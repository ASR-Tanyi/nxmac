#include "py.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include "absl/strings/str_cat.h"

namespace nxmac::shader::internal {
namespace {

std::string Digits(unsigned __int128 v, unsigned base) {
    if (!v) return "0";
    std::string out;
    while (v) {
        out.push_back("0123456789abcdef"[static_cast<unsigned>(v % base)]);
        v /= base;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

unsigned __int128 Magnitude(PyInt v) {
    return v < 0 ? static_cast<unsigned __int128>(0) - static_cast<unsigned __int128>(v)
                 : static_cast<unsigned __int128>(v);
}

// The offset struct.unpack_from reads n bytes at, or struct.error
std::size_t Place(Bytes d, PyInt at, int n) {
    const PyInt size = static_cast<PyInt>(d.size());
    if (at < 0) {
        if (-at > size)
            throw PyError("error", absl::StrCat("offset ", Dec(at), " out of range for ", d.size(), "-byte buffer"));
        at += size;
    }
    if (at + n > size) {
        throw PyError(
            "error", absl::StrCat("unpack_from requires a buffer of at least ", Dec(at + n), " bytes for unpacking ", n,
                                  " bytes at offset ", Dec(at), " (actual buffer size is ", d.size(), ")"));
    }
    return static_cast<std::size_t>(at);
}

template <typename T>
T Little(Bytes d, PyInt at) {
    const std::size_t o = Place(d, at, sizeof(T));
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(static_cast<std::uint8_t>(d[o + i])) << (8 * i);
    return v;
}

template <typename T>
T Big(Bytes d, PyInt at) {
    const std::size_t o = Place(d, at, sizeof(T));
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) v = static_cast<T>(v << 8) | static_cast<std::uint8_t>(d[o + i]);
    return v;
}

// A slice bound: negative from the end, then clamped to [0, size]
std::int64_t Bound(PyInt v, std::int64_t size) {
    if (v < 0) v += size;
    if (v < 0) return 0;
    if (v > size) return size;
    return static_cast<std::int64_t>(v);
}

}  // namespace

void Fail(const std::string& message) { throw Red(message); }

std::string Hex(PyInt value) { return absl::StrCat(value < 0 ? "-0x" : "0x", Digits(Magnitude(value), 16)); }
std::string Dec(PyInt value) { return absl::StrCat(value < 0 ? "-" : "", Digits(Magnitude(value), 10)); }

std::int64_t Len(Bytes d) { return static_cast<std::int64_t>(d.size()); }

std::uint8_t U8(Bytes d, PyInt at) { return Little<std::uint8_t>(d, at); }
std::uint16_t U16(Bytes d, PyInt at) { return Little<std::uint16_t>(d, at); }
std::uint32_t U32(Bytes d, PyInt at) { return Little<std::uint32_t>(d, at); }
std::int32_t I32(Bytes d, PyInt at) { return static_cast<std::int32_t>(Little<std::uint32_t>(d, at)); }
std::uint64_t U64(Bytes d, PyInt at) { return Little<std::uint64_t>(d, at); }
std::int64_t I64(Bytes d, PyInt at) { return static_cast<std::int64_t>(Little<std::uint64_t>(d, at)); }
std::uint32_t U32Be(Bytes d, PyInt at) { return Big<std::uint32_t>(d, at); }
std::uint64_t U64Be(Bytes d, PyInt at) { return Big<std::uint64_t>(d, at); }
double F64(Bytes d, PyInt at) { return std::bit_cast<double>(Little<std::uint64_t>(d, at)); }

void NeedStruct(Bytes d, PyInt at, PyInt n) {
    const PyInt size = static_cast<PyInt>(d.size());
    PyInt o = at;
    if (o < 0) {
        if (-o > size)
            throw PyError("error", absl::StrCat("offset ", Dec(at), " out of range for ", d.size(), "-byte buffer"));
        o += size;
    }
    if (o + n > size) {
        throw PyError("error",
                      absl::StrCat("unpack_from requires a buffer of at least ", Dec(o + n), " bytes for unpacking ",
                                   Dec(n), " bytes at offset ", Dec(o), " (actual buffer size is ", d.size(), ")"));
    }
}

std::uint8_t Index(Bytes d, PyInt at) {
    const PyInt size = static_cast<PyInt>(d.size());
    if (at < 0) at += size;
    if (at < 0 || at >= size) throw PyError("IndexError", "index out of range");
    return static_cast<std::uint8_t>(d[static_cast<std::size_t>(at)]);
}

Bytes Slice(Bytes d, PyInt a, PyInt b) {
    const std::int64_t size = Len(d);
    const std::int64_t lo = Bound(a, size);
    const std::int64_t hi = Bound(b, size);
    if (hi <= lo) return d.substr(0, 0);
    return d.substr(static_cast<std::size_t>(lo), static_cast<std::size_t>(hi - lo));
}

std::int64_t Find(Bytes d, Bytes sub, PyInt start, PyInt end) {
    const std::int64_t size = Len(d);
    if (start > size) return -1;
    const std::int64_t lo = Bound(start, size);
    const std::int64_t hi = Bound(end, size);
    if (hi - lo < Len(sub)) return -1;
    const std::size_t at = d.substr(0, static_cast<std::size_t>(hi)).find(sub, static_cast<std::size_t>(lo));
    return at == Bytes::npos ? -1 : static_cast<std::int64_t>(at);
}

std::int64_t Count(Bytes d, Bytes sub) {
    std::int64_t n = 0;
    FindAll(d, sub, [&](std::int64_t) { ++n; });
    return n;
}

}  // namespace nxmac::shader::internal
