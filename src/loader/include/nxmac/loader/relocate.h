// Relocation of a mapped module against the modules loaded with it, as Horizon's rtld would: RELR's packed relative
// relocations, then DT_RELA and DT_JMPREL (RELATIVE, ABS64 / GLOB_DAT, JUMP_SLOT). A symbol binds to a forced takeover
// first, then to the first loaded module, in load order, that defines it (an earlier module's definition overrides the
// module's own, the ELF default; local and non-default-visibility symbols bind to themselves), then to the host: an
// implementation or a named stub. An undefined weak symbol nothing defines or implements binds to 0. A port of
// nxnative's dm/host/loader.c relocs_of, relr_apply, bind_sym, resolve_sym and relocate; what nxnative's hle.c answers
// comes from the ImportResolver the host gives.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "nxmac/loader/symbols.h"

namespace nxmac::loader {

struct Elf64Rela {
    std::uint64_t r_offset;
    std::uint64_t r_info;  // symbol index << 32 | type
    std::int64_t r_addend;
};
static_assert(sizeof(Elf64Rela) == 24);

enum : std::int64_t { kDtPltRelSz = 2, kDtRela = 7, kDtRelaSz = 8, kDtJmpRel = 23, kDtRelrSz = 35, kDtRelr = 36 };
enum : std::uint32_t {
    kRAarch64Abs64 = 257,
    kRAarch64GlobDat = 1025,
    kRAarch64JumpSlot = 1026,
    kRAarch64Relative = 1027
};

// What relocation asks the host about a name (nxnative's hle.c)
class ImportResolver {
public:
    virtual ~ImportResolver() = default;
    // A forced takeover's address (HLE_FORCE, force_lookup): it overrides every definition; 0 when there is none
    virtual std::uint64_t Forced(std::string_view name) = 0;
    // `name` for `module`, which no loaded module defines: the host's implementation, else a named stub
    // (resolve_import)
    virtual std::uint64_t Import(std::string_view module, std::string_view name) = 0;
    // Whether the host implements `name`, without making a stub for it (hle_implements)
    virtual bool Implements(std::string_view name) = 0;
};

// A loaded module as relocation sees it (module_t's fields it reads)
struct LoadedModule {
    std::string_view name;
    std::uint8_t* base = nullptr;
    std::size_t size = 0;
    std::int64_t patch_off = 0;   // negative: a far module, whose front patch area of patch_len bytes is
    std::uint64_t patch_len = 0;  // before the base and counts as the module's (module_of)
    const ModuleSymbols* symbols = nullptr;
};

struct RelocationReport {
    int relative = 0;  // RELA's R_AARCH64_RELATIVE
    unsigned relr = 0;
    int abs_glob = 0;
    int jump = 0;
    int other = 0;                   // bound to an earlier module's definition of a symbol the module defines too
    int weak0 = 0;                   // undefined weak symbols bound to 0
    std::vector<std::string> lines;  // the lines nxnative's relocate logs, in its order (without its "[dm] ")
};

// Relocates modules[index] against `modules` (every loaded module, in load order: its own definitions are looked up
// there too). InvalidArgument when a relocation table, a relocation's place or its symbol is not inside the image, or a
// RELR bitmap comes before any start (where nxnative's host stops).
absl::StatusOr<RelocationReport> Relocate(std::span<const LoadedModule> modules, std::size_t index,
                                          ImportResolver& resolver);

}  // namespace nxmac::loader
