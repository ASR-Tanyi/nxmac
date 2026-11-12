#include "match.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "nxmac/fs/crypto.h"
#include "text.h"

namespace nxmac::rewriter::internal {
namespace {

// The word some compilers leave after a call that does not return, which Capstone cannot decode; brk #0
constexpr std::uint32_t kTrapWord = 0xe7ffdefe;
constexpr std::uint32_t kBrk = 0xd4200000;

Addr Signed(std::uint64_t n, int bits) {
    return (n & (std::uint64_t{1} << (bits - 1))) != 0 ? static_cast<Addr>(n) - (Addr{1} << bits)
                                                       : static_cast<Addr>(n);
}

struct Relative {
    std::uint32_t mask;  // the immediate's bits
    Addr delta;          // the signed byte displacement
};
// A direct branch's immediate
std::optional<Relative> RelativeOf(std::uint32_t w) {
    if ((w & 0x7c000000) == 0x14000000) return Relative{0x03ffffff, Signed(w & 0x3ffffff, 26) * 4};
    if ((w & 0xff000010) == 0x54000000 || (w & 0x7e000000) == 0x34000000) {
        return Relative{0x00ffffe0, Signed((w >> 5) & 0x7ffff, 19) * 4};
    }
    if ((w & 0x7e000000) == 0x36000000) return Relative{0x0007ffe0, Signed((w >> 5) & 0x3fff, 14) * 4};
    return std::nullopt;
}

std::string Sha256Hex(const void* data, std::size_t size) {
    const fs::Sha256Hash hash = fs::Sha256(std::span(static_cast<const std::uint8_t*>(data), size));
    return BytesHex(hash.data(), hash.size());
}

// A JSON string as Python's json.dumps writes it (ASCII only: the rest as \uXXXX); nullopt for bytes that are not UTF-8
std::optional<std::string> JsonString(std::string_view text) {
    std::string out = "\"";
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += static_cast<char>(c);
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\r') {
                out += "\\r";
            } else if (c == '\t') {
                out += "\\t";
            } else if (c == '\b') {
                out += "\\b";
            } else if (c == '\f') {
                out += "\\f";
            } else if (c < 0x20) {
                out += absl::StrFormat("\\u%04x", c);
            } else {
                out += static_cast<char>(c);
            }
            ++i;
            continue;
        }
        const int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : -1;
        if (extra < 0 || c >= 0xf8 || i + extra >= text.size()) return std::nullopt;
        std::uint32_t point = c & (0x3f >> extra);
        for (int k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0) != 0x80) return std::nullopt;
            point = point << 6 | (next & 0x3f);
        }
        i += extra + 1;
        if (point >= 0x10000) {
            point -= 0x10000;
            out += absl::StrFormat("\\u%04x\\u%04x", 0xd800 + (point >> 10), 0xdc00 + (point & 0x3ff));
        } else {
            out += absl::StrFormat("\\u%04x", point);
        }
    }
    out += '"';
    return out;
}

}  // namespace

std::string PyRepr(std::string_view text) {
    const bool single = absl::StrContains(text, '\'') && !absl::StrContains(text, '"');
    const char quote = single ? '"' : '\'';
    std::string out(1, quote);
    for (const char c : text) {
        if (c == quote || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            out += absl::StrFormat("\\x%02x", static_cast<unsigned char>(c));
        } else {
            out += c;
        }
    }
    out += quote;
    return out;
}

MatchImage::MatchImage(Bytes image, Addr text_end, const Disassembler& md)
    : image_(image), text_end_(text_end), md_(md) {
    std::vector<Range> ranges;
    try {
        ranges = FdeRanges(image);
    } catch (const RewriteError& e) {
        throw MatchError(absl::StrCat("cannot read function boundaries: ", e.what()));
    }
    const Addr limit = std::min<Addr>(text_end, static_cast<Addr>(image.size()));
    for (const auto& [start, end] : ranges) {
        if (!(0 <= start && start < end && end <= limit) || start % 4 != 0 || end % 4 != 0) continue;
        const auto [it, added] = functions_.emplace(start, end);
        if (!added && it->second != end) {
            throw MatchError(absl::StrFormat("conflicting function boundaries at %s", Hex(start)));
        }
    }
}

const MatchImage::Body& MatchImage::BodyOf(Addr start) {
    if (const auto known = bodies_.find(start); known != bodies_.end()) return known->second;
    const Addr end = functions_.at(start);
    const std::size_t count = static_cast<std::size_t>(end - start) / 4;
    std::vector<std::uint32_t> words(count);
    std::memcpy(words.data(), image_.data() + start, count * 4);
    // Decoded as brk so that the rest of the function can be read; the digest below still covers the word itself
    std::vector<std::uint32_t> decode = words;
    for (std::uint32_t& w : decode) {
        if (w == kTrapWord) w = kBrk;
    }
    const std::vector<Insn> ins =
        md_.Disasm(std::span(reinterpret_cast<const std::uint8_t*>(decode.data()), count * 4), start);  // NOLINT
    if (ins.size() != count) throw MatchError(absl::StrFormat("undecodable function at %s", Hex(start)));
    std::vector<std::uint32_t> normalized = words;
    Body body;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t w = words[i];
        const Addr at = ins[i].address;
        if (const std::optional<Relative> branch = RelativeOf(w)) {
            const Addr target = at + branch->delta;
            if (!(start <= target && target < end)) {
                normalized[i] &= ~branch->mask;
                body.branches.emplace_back(static_cast<Addr>(i) * 4, target);
            }
        } else if ((w & 0x1f000000) == 0x10000000) {  // ADR / ADRP
            const Addr delta = Signed(((w >> 29) & 3) | ((w >> 5) & 0x7ffff) << 2, 21);
            if ((w & 0x80000000) != 0 || !(start <= at + delta && at + delta < end)) normalized[i] &= 0x9f00001f;
        } else if ((w & 0x3b000000) == 0x18000000) {  // literal load
            const Addr target = at + Signed((w >> 5) & 0x7ffff, 19) * 4;
            if (!(start <= target && target < end)) normalized[i] &= 0xff00001f;
        }
        if ((w & 0x9f000000) != 0x90000000) continue;
        // The low part of an ADRP address, not an ordinary member offset
        const unsigned rd = w & 31;
        const std::string x = absl::StrCat("x", rd);
        const std::string wn = absl::StrCat("w", rd);
        const std::string_view alias = rd == 29 ? "fp" : rd == 30 ? "lr" : "";
        for (std::size_t j = i + 1; j < std::min(i + 9, count); ++j) {
            const std::uint32_t u = words[j];
            if (((u & 0xffc00000) == 0x91000000 || (u & 0x3b000000) == 0x39000000) && ((u >> 5) & 31) == rd) {
                normalized[j] &= ~std::uint32_t{0x3ffc00};
                break;
            }
            if (RelativeOf(u) || ins[j].IsAny("br", "blr", "ret")) break;
            if (!ins[j].regs_ok) Fail(absl::StrFormat("Capstone lists no registers at %s", Hex(ins[j].address)));
            bool writes = false;
            for (const std::uint16_t r : ins[j].regs_write) {
                const std::string_view name = ins[j].Name(r);
                writes = writes || (!name.empty() && (name == x || name == wn || name == alias));
            }
            if (writes) break;
        }
    }
    body.digest = Sha256Hex(normalized.data(), count * 4);
    return bodies_.emplace(start, std::move(body)).first->second;
}

const std::map<Addr, std::string>& MatchImage::ImportSlots() {
    // GOT slot -> symbol name from DT_JMPREL; unknown formats cannot match
    if (imports_) return *imports_;
    imports_.emplace();
    try {
        const Addr mod = U32(image_, 4);
        const Addr dyn = mod + I32(image_, mod + 4);
        std::map<std::int64_t, std::uint64_t> tags;
        const Addr last = std::min<Addr>(static_cast<Addr>(image_.size()) - 15, dyn + 4096 * 16);
        for (Addr pos = dyn; pos < last; pos += 16) {
            const auto tag = static_cast<std::int64_t>(U64(image_, pos));
            if (tag == 0) break;
            tags[tag] = U64(image_, pos + 8);
        }
        if (!tags.contains(20) || tags[20] != 7) return *imports_;  // DT_PLTREL must be DT_RELA
        if (!tags.contains(23) || !tags.contains(2) || !tags.contains(6) || !tags.contains(5)) throw RangeError("");
        const auto begin = static_cast<Addr>(tags[23]);
        for (Addr pos = begin; pos < begin + static_cast<Addr>(tags[2]); pos += 24) {
            const std::uint64_t slot = U64(image_, pos);
            const std::uint64_t info = U64(image_, pos + 8);
            if ((info & 0xffffffff) != 1026) continue;  // R_AARCH64_JUMP_SLOT
            const Addr name = U32(image_, static_cast<Addr>(tags[6] + (info >> 32) * 24)) + static_cast<Addr>(tags[5]);
            const std::string_view text = CString(image_, name);
            if (!JsonString(text)) throw RangeError("");  // not UTF-8
            (*imports_)[static_cast<Addr>(slot)] = std::string(text);
        }
    } catch (const RangeError&) {
        imports_->clear();
    }
    return *imports_;
}

const std::string* MatchImage::PltImport(Addr target) {
    // AArch64 PLT: adrp x16, page; ldr x17,[x16,#lo]; add x16,x16,#lo; br x17
    if (!(0 <= target && target <= std::min<Addr>(text_end_, static_cast<Addr>(image_.size())) - 16)) return nullptr;
    const std::uint32_t a = U32(image_, target);
    const std::uint32_t b = U32(image_, target + 4);
    const std::uint32_t c = U32(image_, target + 8);
    const std::uint32_t d = U32(image_, target + 12);
    if ((a & 0x9f00001f) != 0x90000010 || (b & 0xffc003ff) != 0xf9400211 || (c & 0xffc003ff) != 0x91000210 ||
        d != 0xd61f0220) {
        return nullptr;
    }
    const Addr page = (target & ~Addr{0xfff}) + Signed(((a >> 29) & 3) | ((a >> 5) & 0x7ffff) << 2, 21) * 0x1000;
    const Addr lo = ((b >> 10) & 0xfff) * 8;
    if (((c >> 10) & 0xfff) != lo) return nullptr;
    const std::map<Addr, std::string>& slots = ImportSlots();
    const auto it = slots.find(page + lo);
    return it == slots.end() || it->second.empty() ? nullptr : &it->second;
}

std::string MatchImage::Callee(Addr target) {
    if (0 <= target && target <= std::min<Addr>(text_end_, static_cast<Addr>(image_.size())) - 16) {
        if (const std::string* name = PltImport(target)) {
            // An import's name was checked to be UTF-8 when the slots were read
            return absl::StrCat("[\"import\",", JsonString(*name).value_or("\"\""), "]");
        }
        // The SDK's system call wrappers are assembly without unwind entries: svc #n; ret, known by its number
        const std::uint32_t a = U32(image_, target);
        if ((a & 0xffe0001f) == 0xd4000001 && U32(image_, target + 4) == 0xd65f03c0 && !functions_.contains(target)) {
            return absl::StrFormat("[\"svc\",%d]", (a >> 5) & 0xffff);
        }
    }
    const auto fn = functions_.find(target);
    if (fn == functions_.end()) {
        throw MatchError(absl::StrFormat("callee has no verified boundary or import at %s", Hex(target)));
    }
    return absl::StrFormat("[\"function\",%d,\"%s\"]", fn->second - target, BodyOf(target).digest);
}

const std::string& MatchImage::Fingerprint(Addr start) {
    if (const auto known = fingerprints_.find(start); known != fingerprints_.end()) return known->second;
    const Body& body = BodyOf(start);
    // json.dumps([body, refs], separators=(',', ':'))
    std::string json = absl::StrCat("[\"", body.digest, "\",[");
    bool first = true;
    for (const auto& [offset, target] : body.branches) {
        absl::StrAppend(&json, first ? "" : ",", "[", offset, ",", Callee(target), "]");
        first = false;
    }
    json += "]]";
    return fingerprints_.emplace(start, Sha256Hex(json.data(), json.size())).first->second;
}

Addr MatchImage::Locate(Addr size, const std::vector<std::string>& fingerprints) {
    std::vector<Addr> matches;
    for (const auto& [start, end] : functions_) {
        if (end - start != size) continue;
        try {
            const std::string& digest = Fingerprint(start);
            if (std::find(fingerprints.begin(), fingerprints.end(), digest) != fingerprints.end())
                matches.push_back(start);
        } catch (const MatchError&) {
            continue;
        }
    }
    if (matches.size() != 1) {
        throw MatchError(absl::StrFormat("%d matching functions (need exactly one)", matches.size()));
    }
    return matches[0];
}

std::vector<Cut> MatchImage::Match(const FunctionRule& rule) {
    const Addr start = Locate(rule.size, rule.fingerprints);
    std::vector<Cut> cuts;
    for (const Cut& cut : rule.cuts) {
        const Addr off = start + cut.offset;
        const std::optional<std::vector<std::uint8_t>> original = HexBytes(cut.original);
        const bool same = original && off >= 0 && static_cast<std::uint64_t>(off) + original->size() <= image_.size() &&
                          std::memcmp(image_.data() + off, original->data(), original->size()) == 0;
        if (!same) throw MatchError(absl::StrFormat("original bytes differ at %s", Hex(off)));
        cuts.push_back({off, cut.original, cut.replacement, cut.reason + " [function match]"});
    }
    return cuts;
}

std::vector<Cut> MatchImage::MatchPatch(const MatchRule& rule) {
    if (!rule.several) return Match(rule.functions[0]);
    std::vector<Cut> cuts;
    std::set<Addr> starts;
    std::vector<std::string> misses;
    const std::size_t n = rule.functions.size();
    for (std::size_t i = 0; i < n; ++i) {
        const FunctionRule& fn = rule.functions[i];
        std::vector<Cut> got;
        try {
            got = Match(fn);
        } catch (const MatchError& e) {
            misses.push_back(absl::StrFormat("function %d of %d (%d bytes): %s", i + 1, n, fn.size, e.what()));
            continue;
        }
        const Addr start = got[0].offset - fn.cuts[0].offset;
        if (starts.contains(start)) {
            misses.push_back(absl::StrFormat("function %d of %d matched a function another one did", i + 1, n));
        }
        starts.insert(start);
        cuts.insert(cuts.end(), got.begin(), got.end());
    }
    if (!misses.empty()) {
        throw MatchError(absl::StrJoin(misses, "; ") +
                         (misses.size() < n ? absl::StrFormat("; the other %d matched", n - misses.size()) : ""));
    }
    return cuts;
}

std::vector<Cut> Resolve(Bytes image, Addr text_end, const std::vector<MatchRule>* rules, const Disassembler& md,
                         bool red, std::vector<std::string>* log, std::vector<MatchOutcome>* outcomes) {
    if (rules == nullptr || rules->empty()) return {};
    // Rules of one name are one patch's reviewed implementations: exactly one of them must match
    std::vector<std::pair<std::string, std::vector<const MatchRule*>>> groups;
    for (const MatchRule& rule : *rules) {
        auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == rule.name; });
        if (group == groups.end()) group = groups.insert(groups.end(), {rule.name, {}});
        group->second.push_back(&rule);
    }
    const auto missed = [&](const std::string& name, const std::string& why) {
        log->push_back(absl::StrCat(red ? "[RED] " : "", "function patch ", PyRepr(name), " skipped: ", why));
        outcomes->push_back({name, std::nullopt, why});
    };
    std::optional<MatchImage> module;
    try {
        module.emplace(image, text_end, md);
    } catch (const MatchError& e) {
        for (const auto& [name, alternatives] : groups) missed(name, e.what());
        return {};
    }
    std::vector<Cut> cuts;
    for (const auto& [name, alternatives] : groups) {
        std::vector<std::pair<std::size_t, std::vector<Cut>>> found;
        std::vector<std::string> misses;
        for (std::size_t i = 0; i < alternatives.size(); ++i) {
            try {
                found.emplace_back(i + 1, module->MatchPatch(*alternatives[i]));
            } catch (const MatchError& e) {
                misses.emplace_back(e.what());
            }
        }
        if (found.size() != 1) {
            missed(name, found.empty() ? absl::StrJoin(misses, "; ")
                                       : absl::StrFormat("%d implementations match (need exactly one)", found.size()));
            continue;
        }
        const auto& [index, got] = found[0];
        std::vector<std::string> at;
        std::vector<Addr> offsets;
        for (const Cut& c : got) {
            at.push_back(Hex(c.offset));
            offsets.push_back(c.offset);
        }
        cuts.insert(cuts.end(), got.begin(), got.end());
        const std::string which = absl::StrFormat("implementation %d of %d", index, alternatives.size());
        log->push_back(absl::StrCat("function patch ", PyRepr(name), ": matched at ", absl::StrJoin(at, ", "),
                                    alternatives.size() > 1 ? absl::StrCat(" (", which, ")") : ""));
        outcomes->push_back({name, std::move(offsets), which});
    }
    std::vector<Range> spans;
    spans.reserve(cuts.size());
    for (const Cut& c : cuts) spans.emplace_back(c.offset, c.offset + static_cast<Addr>(c.original.size() / 2));
    std::sort(spans.begin(), spans.end());
    for (std::size_t i = 1; i < spans.size(); ++i) {
        if (spans[i].first < spans[i - 1].second) Fail("function patches overlap");
    }
    return cuts;
}

}  // namespace nxmac::rewriter::internal
