#include "archive.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

namespace nxmac::shader::internal {
namespace {

bool Cont(std::uint8_t b) { return (b & 0xc0) == 0x80; }

// How CPython's UTF-8 decoder takes the sequence at i: its length when valid, else the bytes one U+FFFD replaces
// (the maximal subpart: the lead byte and the continuation bytes that were right; all the rest at an early end)
struct Utf8Step {
    std::size_t length;
    bool valid;
};

Utf8Step Utf8At(Bytes d, std::size_t i) {
    const auto b = [&](std::size_t k) { return static_cast<std::uint8_t>(d[i + k]); };
    const std::size_t left = d.size() - i;
    const std::uint8_t c = b(0);
    if (c < 0x80) return {1, true};
    int need = 0;
    std::uint8_t lo = 0x80, hi = 0xbf;  // the second byte's range
    if (c >= 0xc2 && c <= 0xdf) {
        need = 1;
    } else if (c >= 0xe0 && c <= 0xef) {
        need = 2;
        if (c == 0xe0) lo = 0xa0;
        if (c == 0xed) hi = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        need = 3;
        if (c == 0xf0) lo = 0x90;
        if (c == 0xf4) hi = 0x8f;
    } else {
        return {1, false};  // invalid start byte
    }
    for (int k = 1; k <= need; ++k) {
        if (static_cast<std::size_t>(k) >= left) return {left, false};  // unexpected end of data: the rest
        const std::uint8_t x = b(static_cast<std::size_t>(k));
        const bool ok = k == 1 ? (x >= lo && x <= hi) : Cont(x);
        if (!ok) return {static_cast<std::size_t>(k), false};
    }
    return {static_cast<std::size_t>(need) + 1, true};
}

}  // namespace

std::string DecodeUtf8Replace(Bytes d) {
    std::string out;
    out.reserve(d.size());
    for (std::size_t i = 0; i < d.size();) {
        const Utf8Step s = Utf8At(d, i);
        if (s.valid) {
            out.append(d.substr(i, s.length));
        } else {
            out.append("\xef\xbf\xbd");
        }
        i += s.length;
    }
    return out;
}

bool ValidUtf8(Bytes d) {
    for (std::size_t i = 0; i < d.size();) {
        const Utf8Step s = Utf8At(d, i);
        if (!s.valid) return false;
        i += s.length;
    }
    return true;
}

std::string Yaz0(Bytes s, const std::string& label) {
    // n from the header as int.from_bytes(s[4:8], 'big'): a shorter header gives fewer bytes
    std::uint64_t n = 0;
    for (const char c : Slice(s, 4, 8)) n = n << 8 | static_cast<std::uint8_t>(c);
    std::string out;
    out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(n, std::uint64_t(1) << 32)));
    std::size_t si = 16;
    const auto ended = [&]() {
        Fail(absl::StrCat(label, ": the Yaz0 stream ends at ", Hex(Len(s)), " with ",
                          Hex(static_cast<PyInt>(out.size())), " of its ", Hex(static_cast<PyInt>(n)),
                          " bytes decoded"));
    };
    while (out.size() < n) {
        if (si >= s.size()) ended();
        std::uint8_t h = static_cast<std::uint8_t>(s[si++]);
        for (int bit = 0; bit < 8; ++bit) {
            if (h & 0x80) {
                if (si >= s.size()) ended();
                out.push_back(s[si++]);
            } else {
                if (si + 1 >= s.size()) ended();
                const std::uint8_t b1 = static_cast<std::uint8_t>(s[si]), b2 = static_cast<std::uint8_t>(s[si + 1]);
                si += 2;
                const std::size_t dist = (static_cast<std::size_t>(b1 & 15) << 8 | b2) + 1;
                std::size_t ln = b1 >> 4;
                if (ln) {
                    ln += 2;
                } else {
                    if (si >= s.size()) ended();
                    ln = static_cast<std::uint8_t>(s[si++]) + 0x12;
                }
                if (dist > out.size()) {
                    Fail(absl::StrCat(label, ": a back-reference at ", Hex(static_cast<PyInt>(si)), " reaches ",
                                      Hex(static_cast<PyInt>(dist - out.size())), " bytes before the start"));
                }
                const std::size_t st = out.size() - dist;
                if (dist >= ln) {
                    out.append(out, st, ln);
                } else {  // overlapping: the period repeats
                    for (std::size_t k = 0; k < ln; ++k) out.push_back(out[st + k]);
                }
            }
            if (out.size() >= n) break;
            h = static_cast<std::uint8_t>(h << 1);
        }
    }
    out.resize(static_cast<std::size_t>(n));
    return out;
}

void SarcMembers(Bytes d, const std::string& label, const std::function<void(const std::string&, Bytes)>& f) {
    const auto field = [&](PyInt at, PyInt size, const std::string& where) {
        if (!(0 <= at && at <= Len(d) - size)) {
            Fail(absl::StrCat(where, ": ", Hex(size), " bytes at ", Hex(at), " lie outside the ", Hex(Len(d)),
                              " bytes there are"));
        }
    };
    field(4, 12, absl::StrCat(label, " SARC header"));
    const PyInt hl = U16(d, 4), doff = U32(d, 12);
    field(hl + 6, 2, absl::StrCat(label, " SFAT header"));
    const PyInt n = U16(d, hl + 6);
    const PyInt names = hl + 12 + 16 * n + 8;
    if (!(names <= doff && doff <= Len(d))) {
        Fail(absl::StrCat(label, ": SARC data at ", Hex(doff), ", its ", Dec(n), " entries and the names end at ",
                          Hex(names), ", the archive at ", Hex(Len(d))));
    }
    for (PyInt i = 0; i < n; ++i) {
        const PyInt at = hl + 12 + 16 * i;
        const PyInt attr = U32(d, at + 4), s = U32(d, at + 8), e = U32(d, at + 12);
        if (!(s <= e && e <= Len(d) - doff)) {
            Fail(absl::StrCat(label, ": SARC member ", Dec(i), " at ", Hex(s), "..", Hex(e),
                              " of the data lies outside its ", Hex(Len(d) - doff), " bytes"));
        }
        std::string name;
        if (attr >> 24) {  // up to the name's own NUL
            const PyInt o = names + (attr & 0xffffff) * 4;
            const std::int64_t z = Find(d, Lit("\0"), o);
            name = DecodeUtf8Replace(Slice(d, o, z >= 0 ? z : Len(d)));
        } else {
            name = absl::StrCat("#", Dec(i));
        }
        f(name, Slice(d, doff + s, doff + e));
    }
}

Inflated Inflate(Bytes in, std::size_t max_out) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) throw PyError("error", "Error while creating decompression object");
    Inflated r;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    z.avail_in = 0;
    std::size_t fed = 0;
    int err = Z_OK;
    for (;;) {
        if (!z.avail_in && fed < in.size()) {  // zlib takes at most 4 GiB at a time
            const std::size_t chunk = std::min<std::size_t>(in.size() - fed, 1u << 30);
            z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data() + fed));
            z.avail_in = static_cast<uInt>(chunk);
            fed += chunk;
        }
        std::size_t room = 1 << 16;
        if (max_out) {
            if (r.out.size() >= max_out) break;
            room = std::min(room, max_out - r.out.size());
        }
        const std::size_t had = r.out.size();
        r.out.resize(had + room);
        z.next_out = reinterpret_cast<Bytef*>(r.out.data() + had);
        z.avail_out = static_cast<uInt>(room);
        err = inflate(&z, Z_SYNC_FLUSH);
        r.out.resize(had + room - z.avail_out);
        if (err == Z_STREAM_END) {
            r.eof = true;
            break;
        }
        if (err == Z_BUF_ERROR || (err == Z_OK && !z.avail_in && fed == in.size() && z.avail_out)) {
            err = Z_OK;  // all the input is in: the stream goes on in bytes not given
            break;
        }
        if (err != Z_OK) break;
    }
    const std::string why = z.msg ? z.msg : "";
    const std::size_t left = z.avail_in + (in.size() - fed);
    inflateEnd(&z);
    if (err != Z_OK && err != Z_STREAM_END) {
        throw PyError("error", absl::StrCat("Error ", err, " while decompressing data", why.empty() ? "" : ": ", why));
    }
    if (r.eof) r.unused = left;
    return r;
}

std::optional<Inflated> TryInflate(Bytes in, std::size_t max_out) {
    try {
        return Inflate(in, max_out);
    } catch (const PyError&) {
        return std::nullopt;
    }
}

std::string PyBytesRepr(Bytes b) {
    // repr(bytes): single quotes unless the bytes hold one and no double quote
    const bool dq = absl::StrContains(b, '\'') && !absl::StrContains(b, '"');
    std::string out = dq ? "b\"" : "b'";
    for (const char ch : b) {
        const auto c = static_cast<std::uint8_t>(ch);
        if (c == '\\' || (c == '\'' && !dq)) {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c < 0x20 || c >= 0x7f) {
            absl::StrAppend(&out, "\\x", absl::Hex(c, absl::kZeroPad2));
        } else {
            out += static_cast<char>(c);
        }
    }
    out += dq ? '"' : '\'';
    return out;
}

std::string Ungzip(Bytes d, const std::string& label) {
    // gzip.GzipFile(...).read(limit + 1): every member, its header, its deflate stream and its CRC and length checked,
    // zero padding between members; the budget: deflate reaches about 1000x on repetitive data
    const std::uint64_t limit =
        std::max<std::uint64_t>(std::uint64_t(1) << 30, static_cast<std::uint64_t>(kNest) * d.size());
    const auto bad = [&](const std::string& why) { Fail(absl::StrCat(label, ": invalid gzip stream (", why, ")")); };
    const std::string ended = "Compressed file ended before the end-of-stream marker was reached";
    std::string out;
    std::size_t at = 0;
    const auto need = [&](std::size_t n) {
        if (d.size() - at < n) bad(ended);
    };
    while (out.size() <= limit) {
        const Bytes magic = d.substr(at, 2);
        if (magic.empty()) break;
        if (magic != Lit("\x1f\x8b")) bad(absl::StrCat("Not a gzipped file (", PyBytesRepr(magic), ")"));
        at += 2;
        need(8);
        const auto method = static_cast<std::uint8_t>(d[at]);
        const auto flag = static_cast<std::uint8_t>(d[at + 1]);
        at += 8;
        if (method != 8) bad("Unknown compression method");
        if (flag & 4) {  // FEXTRA
            need(2);
            const std::size_t n =
                static_cast<std::uint8_t>(d[at]) | static_cast<std::size_t>(static_cast<std::uint8_t>(d[at + 1])) << 8;
            at += 2;
            need(n);
            at += n;
        }
        for (const int f : {8, 16}) {  // FNAME, FCOMMENT: up to a NUL or the end
            if (!(flag & f)) continue;
            while (at < d.size()) {
                const char c = d[at];
                ++at;
                if (c == '\0') break;
            }
        }
        if (flag & 2) {  // FHCRC, not checked
            need(2);
            at += 2;
        }
        z_stream z{};
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) bad("zlib");
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(d.data() + at));
        z.avail_in = static_cast<uInt>(std::min<std::size_t>(d.size() - at, 0xffffffffu));
        const std::size_t start = out.size();
        int err = Z_OK;
        while (out.size() <= limit) {
            const std::size_t room = static_cast<std::size_t>(std::min<std::uint64_t>(1 << 20, limit + 1 - out.size()));
            const std::size_t had = out.size();
            out.resize(had + room);
            z.next_out = reinterpret_cast<Bytef*>(out.data() + had);
            z.avail_out = static_cast<uInt>(room);
            err = inflate(&z, Z_NO_FLUSH);
            out.resize(had + room - z.avail_out);
            if (err != Z_OK) break;
        }
        const std::string why = z.msg ? z.msg : "";
        at += (d.size() - at) - z.avail_in;
        inflateEnd(&z);
        if (out.size() > limit) break;
        if (err == Z_BUF_ERROR) bad(ended);
        if (err != Z_STREAM_END)
            bad(absl::StrCat("Error ", err, " while decompressing data", why.empty() ? "" : ": ", why));
        need(8);
        const std::uint32_t stored_crc = U32(d, static_cast<PyInt>(at)), isize = U32(d, static_cast<PyInt>(at) + 4);
        at += 8;
        const Bytes member = Bytes(out).substr(start);
        uLong crc = crc32(0, nullptr, 0);
        for (std::size_t o = 0; o < member.size(); o += 1u << 30) {
            const std::size_t n = std::min<std::size_t>(member.size() - o, 1u << 30);
            crc = crc32(crc, reinterpret_cast<const Bytef*>(member.data() + o), static_cast<uInt>(n));
        }
        if (stored_crc != static_cast<std::uint32_t>(crc)) {
            bad(absl::StrCat("CRC check failed ", Hex(stored_crc), " != ", Hex(static_cast<std::uint32_t>(crc))));
        }
        if (isize != static_cast<std::uint32_t>(member.size())) bad("Incorrect length of data produced");
        while (at < d.size() && d[at] == '\0') ++at;  // zero padding between members
    }
    if (out.size() > limit)
        Fail(absl::StrCat(label, ": gzip stream expands beyond its ", Hex(static_cast<PyInt>(limit)), " byte budget"));
    return out;
}

std::optional<std::string> GustGz(Bytes d) {
    std::int64_t o = 0;
    std::string out;
    bool any = false;
    while (o + 4 <= Len(d)) {
        const std::int64_t n = U32(d, o);
        o += 4;
        if (!n) {
            if (any && o == Len(d)) return out;
            return std::nullopt;
        }
        if (o + n > Len(d) || static_cast<std::uint8_t>(d[static_cast<std::size_t>(o)]) != 0x78) return std::nullopt;
        const std::optional<Inflated> z = TryInflate(Slice(d, o, o + n));
        if (!z || !z->eof || z->unused) return std::nullopt;
        out += z->out;
        any = true;
        o += n;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> FrameSize(Bytes f) {
    const auto le = [&](PyInt o, PyInt n) {
        std::uint64_t v = 0;
        const Bytes b = Slice(f, o, o + n);
        for (std::size_t i = b.size(); i-- > 0;) v = v << 8 | static_cast<std::uint8_t>(b[i]);
        return v;
    };
    if (Head(f, 4) == Lit("\x04\x22\x4d\x18")) {  // LZ4: FLG, BD, then the content size when FLG bit 3 is set
        if (Len(f) > 14 && (static_cast<std::uint8_t>(f[4]) & 8)) return le(6, 8);
        return std::nullopt;
    }
    const std::uint8_t fhd = Len(f) > 5 ? static_cast<std::uint8_t>(f[4]) : 0;
    const int single = fhd >> 5 & 1;
    static constexpr int kSizes[] = {0, 2, 4, 8};
    const int n = (fhd >> 6) == 0 ? single : kSizes[fhd >> 6];
    if (!n) return std::nullopt;
    static constexpr int kDict[] = {0, 1, 2, 4};
    const int o = 6 - single + kDict[fhd & 3];
    return le(o, n) + (n == 2 ? 256 : 0);
}

}  // namespace nxmac::shader::internal
