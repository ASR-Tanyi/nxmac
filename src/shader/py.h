// The Python semantics the shader split's readers rely on (internal to nxmac::shader). nxnative's shader_split.py reads
// bytes with struct.unpack_from, slices and bytes.find, and stops with red(); its readers are ported line by line, so
// these helpers do what those do: an unpack outside the bytes throws as struct.error does, a slice clamps (and counts
// a negative index from the end), and the numbers that can come from a file are Python's unbounded integers, held in
// 128 bits (a u64 pointer plus a base never overflows them).
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace nxmac::shader::internal {

using Bytes = std::string_view;  // bytes, as Python's bytes: the split's buffers are std::string
using PyInt = __int128;          // an int as Python's: a u64 field plus an offset fits

// red(): a [RED] stop of shader_split.py (the message without "[RED] ")
class Red : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A reader of shader_split.py that nxmac has not ported (another engine's archive or container): the file is left out
// as an unreadable one is, or the split stops when it is the game's main or its whole RomFS
class NotPorted : public Red {
public:
    using Red::Red;
};

// An exception a Python reader would raise on damaged data (struct.error, IndexError, zlib.error, ...); type is the
// Python class name the split's red line shows
class PyError : public std::runtime_error {
public:
    PyError(std::string type, const std::string& message) : std::runtime_error(message), type_(std::move(type)) {}
    const std::string& type() const { return type_; }

private:
    std::string type_;
};

[[noreturn]] void Fail(const std::string& message);  // throws Red

// f'{value:#x}': 0x0, 0x1a, -0x10
std::string Hex(PyInt value);
// f'{value}'
std::string Dec(PyInt value);

// struct.unpack_from of one little-endian (or, with Be, big-endian) field at offset at; a negative offset counts from
// the end, as Python's does. struct.error (PyError) when it does not lie inside d.
std::uint8_t U8(Bytes d, PyInt at);
std::uint16_t U16(Bytes d, PyInt at);
std::uint32_t U32(Bytes d, PyInt at);
std::int32_t I32(Bytes d, PyInt at);
std::uint64_t U64(Bytes d, PyInt at);
std::int64_t I64(Bytes d, PyInt at);
std::uint32_t U32Be(Bytes d, PyInt at);
std::uint64_t U64Be(Bytes d, PyInt at);
double F64(Bytes d, PyInt at);
// struct.unpack_from's check alone: n bytes at at (the whole of a multi-field format)
void NeedStruct(Bytes d, PyInt at, PyInt n);

// d[at], IndexError when it is not inside d (a negative index counts from the end)
std::uint8_t Index(Bytes d, PyInt at);

// d[a:b]: clamped to d, a negative bound counting from the end
Bytes Slice(Bytes d, PyInt a, PyInt b);
inline Bytes From(Bytes d, PyInt a) { return Slice(d, a, static_cast<PyInt>(d.size())); }
inline Bytes Head(Bytes d, PyInt n) { return Slice(d, 0, n); }

// d.find(sub, start, end): -1 when absent; start and end as slice bounds
std::int64_t Find(Bytes d, Bytes sub, PyInt start = 0, PyInt end = PyInt(1) << 100);
inline bool Contains(Bytes d, Bytes sub) { return Find(d, sub) >= 0; }
// d.count(sub) (non-overlapping)
std::int64_t Count(Bytes d, Bytes sub);

// The start of every match of re.finditer(re.escape(pattern), d), in order: the leftmost, then the next from its end
// (as d.count counts them)
template <typename F>
void FindAll(Bytes d, Bytes pattern, const F& f) {
    for (std::size_t at = d.find(pattern); at != Bytes::npos; at = d.find(pattern, at + pattern.size())) {
        f(static_cast<std::int64_t>(at));
    }
}

// The bytes of a literal with NULs in it: Lit("BNSH\0\0\0\0")
template <std::size_t N>
constexpr Bytes Lit(const char (&s)[N]) {
    return {s, N - 1};
}

std::int64_t Len(Bytes d);

}  // namespace nxmac::shader::internal
