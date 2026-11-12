#include "image.h"

#include <algorithm>

#include "absl/strings/str_format.h"
#include "text.h"

namespace nxmac::rewriter::internal {
namespace {

constexpr Addr kDataMin = 0x1000;

struct Value {
    Addr value;
    Addr size;
};

// (eh_frame_hdr offset, its end) from the MOD0 header
Range EhFrameHdr(Bytes image) {
    const Addr m0 = U32(image, 4);
    if (U32(image, m0) != 0x30444F4D) Fail("no MOD0");
    return {m0 + I32(image, m0 + 16), m0 + I32(image, m0 + 20)};
}

// A DWARF pointer of encoding enc at o (nso_code.py _read)
Value ReadPointer(Bytes image, Addr o, unsigned enc, Addr ehs) {
    const unsigned format = enc & 0xF;
    const unsigned applied = enc & 0x70;
    Value v{};
    if (format == 0x3) {
        v = {U32(image, o), 4};
    } else if (format == 0xB) {
        v = {I32(image, o), 4};
    } else if (format == 0x0 || format == 0x4 || format == 0xC) {
        v = {static_cast<Addr>(U64(image, o)), 8};
    } else {
        Fail(absl::StrFormat("pointer encoding %s at %s", Hex(static_cast<std::int64_t>(enc)), Hex(o)));
    }
    if (applied == 0x10) v.value += o;
    if (applied == 0x30) v.value += ehs;
    return v;
}

Addr SkipUleb(Bytes image, Addr o) {
    while ((U8(image, o++) & 0x80) != 0) {
    }
    return o;
}

// [(function start, FDE offset)] from the eh_frame_hdr binary search table; empty without a valid eh_frame_hdr
std::vector<Range> FdeTable(Bytes image) {
    const auto [ehs, ehe] = EhFrameHdr(image);
    if (ehe - ehs < 12 || U8(image, ehs) != 1 || U8(image, ehs + 3) == 0xFF || U8(image, ehs + 3) == 0) return {};
    const unsigned penc = U8(image, ehs + 1);
    const unsigned cenc = U8(image, ehs + 2);
    const unsigned tenc = U8(image, ehs + 3);
    Addr o = ehs + 4;
    o += ReadPointer(image, o, penc, ehs).size;
    const Value count = ReadPointer(image, o, cenc, ehs);
    o += count.size;
    std::vector<Range> out;
    for (Addr i = 0; i < count.value; ++i) {
        const Value a = ReadPointer(image, o, tenc, ehs);
        o += a.size;
        const Value f = ReadPointer(image, o, tenc, ehs);
        o += f.size;
        out.emplace_back(a.value, f.value);
    }
    return out;
}

}  // namespace

void FailRange(Addr at, std::size_t size) {
    throw RangeError(absl::StrFormat("a read of %d bytes at %s is outside the image", size, Hex(at)));
}

std::string_view CString(Bytes image, Addr at) {
    if (at < 0 || static_cast<std::uint64_t>(at) > image.size()) FailRange(at, 1);
    const void* nul = std::memchr(image.data() + at, 0, image.size() - static_cast<std::size_t>(at));
    if (nul == nullptr) FailRange(at, 1);
    const auto* begin = reinterpret_cast<const char*>(image.data() + at);  // NOLINT: bytes as chars
    return {begin, static_cast<std::size_t>(static_cast<const char*>(nul) - begin)};
}

std::vector<Addr> FunctionStarts(Bytes image, Addr text_end) {
    const Addr m0 = U32(image, 4);
    if (U32(image, m0) != 0x30444F4D) Fail("the module has no MOD0 header");
    const Addr ehs = m0 + I32(image, m0 + 16);
    // patch_nso.py reads this table itself, in the three formats modules have: udata4, sdata4, udata8
    const auto read = [&](Addr o, unsigned enc) {
        const unsigned format = enc & 0xF;
        Value v{};
        if (format == 0x3) {
            v = {U32(image, o), 4};
        } else if (format == 0xB) {
            v = {I32(image, o), 4};
        } else if (format == 0x4) {
            v = {static_cast<Addr>(U64(image, o)), 8};
        } else {
            Fail(absl::StrFormat("the module's .eh_frame_hdr has pointer encoding %s, which the rewriter does not read",
                                 Hex(static_cast<std::int64_t>(enc))));
        }
        if ((enc & 0x70) == 0x10) v.value += o;
        if ((enc & 0x70) == 0x30) v.value += ehs;
        return v;
    };
    const unsigned penc = U8(image, ehs + 1);
    const unsigned cenc = U8(image, ehs + 2);
    const unsigned tenc = U8(image, ehs + 3);
    Addr o = ehs + 4;
    o += read(o, penc).size;
    const Value count = read(o, cenc);
    o += count.size;
    std::vector<Addr> starts;
    for (Addr i = 0; i < count.value; ++i) {
        const Value a = read(o, tenc);
        o += a.size;
        o += read(o, tenc).size;
        if (a.value < text_end) starts.push_back(a.value);
    }
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    return starts;
}

std::vector<Range> FdeRanges(Bytes image) {
    const Addr ehs = EhFrameHdr(image).first;
    std::map<Addr, unsigned> cies;
    // The encoding of an FDE's addresses, from its CIE's augmentation data
    const auto cie_encoding = [&](Addr cie) {
        if (const auto known = cies.find(cie); known != cies.end()) return known->second;
        Addr p = cie + 8;
        const unsigned version = U8(image, p++);
        const std::string_view augmentation = CString(image, p);
        p += static_cast<Addr>(augmentation.size()) + 1;
        p = SkipUleb(image, p);                  // code alignment
        while ((U8(image, p) & 0x80) != 0) ++p;  // data alignment (sleb)
        ++p;
        p = version == 1 ? p + 1 : SkipUleb(image, p);  // return address register
        unsigned enc = 0;
        if (!augmentation.empty() && augmentation[0] == 'z') {
            p = SkipUleb(image, p);
            for (const char ch : augmentation.substr(1)) {
                if (ch == 'R') {
                    enc = U8(image, p++);
                } else if (ch == 'P') {
                    const unsigned pe = U8(image, p++);
                    p += ReadPointer(image, p, pe, ehs).size;
                } else if (ch == 'L') {
                    ++p;
                }
            }
        }
        cies[cie] = enc;
        return enc;
    };
    std::vector<Range> out;
    for (const auto& [start_unused, fde] : FdeTable(image)) {
        const Addr cptr = fde + 4;
        const unsigned enc = cie_encoding(cptr - U32(image, cptr));
        const Value start = ReadPointer(image, fde + 8, enc, ehs);
        const Value length = ReadPointer(image, fde + 8 + start.size, enc & 0xF, ehs);
        out.emplace_back(start.value, start.value + length.value);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Range> DataBlocks(Bytes image, Addr text_end, const Disassembler& md) {
    // An NRO's text includes its header: the build ID at +0x40 is arbitrary bytes, not instructions. The entry branch
    // and MOD0 pointer (+0..+8) stay code; the reserved word and the NRO0 header (+8..+0x80) are never scanned.
    const bool nro = image.size() >= 0x14 && std::memcmp(image.data() + 0x10, "NRO0", 4) == 0;
    std::vector<Range> gaps;
    Addr last = nro ? 0x80 : 0;
    for (const auto& [s, e] : FdeRanges(image)) {
        if (s >= text_end) break;
        if (s > last) gaps.emplace_back(last, s);
        last = std::max(last, e);
    }
    if (last < text_end) gaps.emplace_back(last, text_end);
    std::vector<Range> out;
    if (nro) out.emplace_back(8, 0x80);
    for (auto [s, e] : gaps) {
        s = (s + 3) & ~Addr{3};
        e = e & ~Addr{3};
        if (e - s < kDataMin) continue;
        Addr undecodable = 0;
        for (Addr a = s; a < e; a += 4) {
            if (!md.Lite(U32(image, a), a)) ++undecodable;
        }
        if (undecodable * 4 > (e - s) / 4) out.emplace_back(s, e);
    }
    return out;
}

OutStore FindOutStore(Bytes image, Addr start, Addr size, const Disassembler& md) {
    OutStore result;
    if (start < 0 || size < 0 || static_cast<std::uint64_t>(start) > image.size()) {
        result.error = "the function lies outside the image";
        return result;
    }
    const std::size_t length = std::min<std::size_t>(static_cast<std::size_t>(size), image.size() - start);
    const std::vector<Insn> ins = md.Disasm(image.subspan(static_cast<std::size_t>(start), length), start);
    std::vector<const Insn*> copies;
    for (std::size_t i = 0; i < ins.size() && i < 16; ++i) {
        if (ins[i].Is("mov") && Scan(ins[i].op_str, "%x, x1")) copies.push_back(&ins[i]);
    }
    if (copies.size() != 1) {
        result.error = "no single copy of the out pointer in the prologue";
        return result;
    }
    const std::string xb(copies[0]->op_str.substr(0, copies[0]->op_str.find(',')));
    std::vector<const Insn*> stores;
    const std::string pattern = "%x, [" + xb + "]";
    for (const Insn& i : ins) {
        if (i.Is("str") && Scan(i.op_str, pattern)) stores.push_back(&i);
    }
    if (stores.size() != 1) {
        result.error = absl::StrFormat("%d stores through the out pointer", stores.size());
        return result;
    }
    result.offset = stores[0]->address;
    result.reg =
        static_cast<int>(ParseInt(std::string_view(stores[0]->op_str).substr(1, stores[0]->op_str.find(',') - 1)));
    return result;
}

std::map<std::string, Range, std::less<>> DefinedSymbols(Bytes image, std::span<const std::string_view> names) {
    std::map<std::string, Range, std::less<>> out;
    try {
        const Addr mod0 = U32(image, 4);
        if (mod0 + 4 > static_cast<Addr>(image.size()) || std::memcmp(image.data() + mod0, "MOD0", 4) != 0) return {};
        const Addr dyn = mod0 + I32(image, mod0 + 4);
        std::map<std::int64_t, std::uint64_t> tags;
        for (Addr i = 0; i < 4096; ++i) {
            const auto tag = static_cast<std::int64_t>(U64(image, dyn + i * 16));
            if (tag == 0) break;
            tags.emplace(tag, U64(image, dyn + i * 16 + 8));  // the first of each tag
        }
        if (!tags.contains(6) || !tags.contains(5)) return {};
        const auto symtab = static_cast<Addr>(tags[6]);
        const auto strtab = static_cast<Addr>(tags[5]);
        const Addr count = tags.contains(4) ? U32(image, static_cast<Addr>(tags[4]) + 4) : (strtab - symtab) / 24;
        for (Addr i = 1; i < count; ++i) {
            const Addr at = symtab + i * 24;
            const std::uint32_t name = U32(image, at);
            const std::uint16_t shndx = Load<std::uint16_t>(image, at + 6);
            const std::uint64_t value = U64(image, at + 8);
            const std::uint64_t size = U64(image, at + 16);
            if (shndx == 0) continue;
            const std::string_view text = CString(image, strtab + name);
            if (std::find(names.begin(), names.end(), text) == names.end()) continue;
            out.insert_or_assign(std::string(text), Range{static_cast<Addr>(value), static_cast<Addr>(size)});
        }
    } catch (const RangeError&) {
        return {};
    }
    return out;
}

}  // namespace nxmac::rewriter::internal
