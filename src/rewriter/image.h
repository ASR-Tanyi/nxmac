// What a module's image says about itself (internal to nxmac::rewriter): checked reads, the MOD0 header, function
// boundaries from .eh_frame, data blocks in the text, and the dynamic symbol table. Ports of the readers in nxnative's
// patch_nso.py, nso_code.py and nso_syms.py.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "disasm.h"
#include "error.h"

namespace nxmac::rewriter::internal {

using Addr = std::int64_t;
using Range = std::pair<Addr, Addr>;  // [first, second)
using Bytes = std::span<const std::uint8_t>;

// A read past the image: a table that points nowhere. Readers that survive a module without a table catch it.
class RangeError : public RewriteError {
public:
    using RewriteError::RewriteError;
};
[[noreturn]] void FailRange(Addr at, std::size_t size);

template <typename T>
T Load(Bytes image, Addr at) {
    if (at < 0 || static_cast<std::uint64_t>(at) + sizeof(T) > image.size()) FailRange(at, sizeof(T));
    T value;
    std::memcpy(&value, image.data() + at, sizeof(T));  // the host is little-endian, as the guest is
    return value;
}
inline std::uint32_t U32(Bytes image, Addr at) { return Load<std::uint32_t>(image, at); }
inline std::int32_t I32(Bytes image, Addr at) { return Load<std::int32_t>(image, at); }
inline std::uint64_t U64(Bytes image, Addr at) { return Load<std::uint64_t>(image, at); }
inline std::uint8_t U8(Bytes image, Addr at) { return Load<std::uint8_t>(image, at); }
inline void PutU32(std::span<std::uint8_t> image, Addr at, std::uint32_t value) {
    if (at < 0 || static_cast<std::uint64_t>(at) + 4 > image.size()) FailRange(at, 4);
    std::memcpy(image.data() + at, &value, 4);
}
// The bytes up to the NUL at or after `at`; RangeError when there is none
std::string_view CString(Bytes image, Addr at);

// The starts of the functions .eh_frame_hdr lists, below text_end, sorted (patch_nso.py "Function boundaries"). A
// RewriteError for a module without MOD0 or with a table in an encoding the rewriter does not read.
std::vector<Addr> FunctionStarts(Bytes image, Addr text_end);

// Sorted [start, end) of every function the FDEs describe (nso_code.py fde_ranges)
std::vector<Range> FdeRanges(Bytes image);

// Sorted data blocks in the text [0, text_end): stretches of at least 0x1000 bytes between FDE ranges in which more
// than a quarter of the words do not decode (nso_code.py data_blocks). An NRO's header (+8..+0x80) is always one.
std::vector<Range> DataBlocks(Bytes image, Addr text_end, const Disassembler& md);

// The one store through a function's second argument, an out pointer copied once in the prologue (mov xB, x1 ...
// str xA, [xB]): its offset and A. The error text when that shape is not there (nso_code.py out_store).
struct OutStore {
    Addr offset = 0;
    int reg = 0;
    std::string error;
};
OutStore FindOutStore(Bytes image, Addr start, Addr size, const Disassembler& md);

// {name: (offset, size)} of the module's defined dynamic symbols among names; empty without a symbol table it can
// read, as a 32-bit module's is (nso_syms.py defined)
std::map<std::string, Range, std::less<>> DefinedSymbols(Bytes image, std::span<const std::string_view> names);

}  // namespace nxmac::rewriter::internal
