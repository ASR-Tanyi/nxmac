// A mapped module's dynamic section and its symbols: where MOD0 points, what DT_SYMTAB, DT_STRTAB, DT_INIT, DT_FINI and
// DT_INIT_ARRAY say, and lookups by name through a hash table that gives what a linear scan gives (the first defined
// symbol of that name). A port of nxnative's dm/host/loader.c parse_dynamic, build_symhash, module_sym and
// module_sym_linear, with the ELF structures of dm/host/dm.h.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace nxmac::loader {

struct Elf64Sym {
    std::uint32_t st_name;
    std::uint8_t st_info;
    std::uint8_t st_other;
    std::uint16_t st_shndx;  // 0: undefined (an import)
    std::uint64_t st_value;
    std::uint64_t st_size;
};
static_assert(sizeof(Elf64Sym) == 24);

struct Elf64Dyn {
    std::int64_t d_tag;
    std::uint64_t d_val;  // d_val or d_ptr
};
static_assert(sizeof(Elf64Dyn) == 16);

enum : std::int64_t {
    kDtStrtab = 5,
    kDtSymtab = 6,
    kDtInit = 12,
    kDtFini = 13,
    kDtInitArray = 25,
    kDtInitArraySz = 27,
};

class ModuleSymbols {
public:
    // The image at `base` (`size` bytes, as mapped): its MOD0 header's offset is at +4, the dynamic section's offset is
    // MOD0's +4 from MOD0. InvalidArgument when there is no MOD0 (nxnative's host stops there too), or when the dynamic
    // section or the tables it names are not inside the image.
    static absl::StatusOr<ModuleSymbols> Parse(std::uint8_t* base, std::size_t size, std::string_view name);

    const Elf64Dyn* dyn() const { return dyn_; }
    const Elf64Sym* symtab() const { return symtab_; }
    const char* strtab() const { return strtab_; }
    unsigned nsym() const { return nsym_; }       // the symbols before the string table (index 0 is the null symbol)
    std::uint64_t init() const { return init_; }  // DT_INIT: the module's _init (registers it, runs its init_array)
    std::uint64_t fini() const { return fini_; }  // DT_FINI (nn::ro::UnloadModule calls it)
    std::uint64_t init_array() const { return init_array_; }
    std::uint64_t init_arraysz() const { return init_arraysz_; }
    std::size_t hash_slots() const { return symh_.size(); }

    // The address of the first defined symbol named `name`; null when there is none (module_sym)
    std::uint8_t* Find(std::string_view name) const;
    // The same by scanning the table (module_sym_linear): DM_SYMCHECK's reference
    std::uint8_t* FindLinear(std::string_view name) const;

private:
    const char* NameOf(std::uint32_t index) const { return strtab_ + symtab_[index].st_name; }

    std::uint8_t* base_ = nullptr;
    const Elf64Dyn* dyn_ = nullptr;
    const Elf64Sym* symtab_ = nullptr;
    const char* strtab_ = nullptr;
    unsigned nsym_ = 0;
    std::uint64_t init_ = 0;
    std::uint64_t fini_ = 0;
    std::uint64_t init_array_ = 0;
    std::uint64_t init_arraysz_ = 0;
    // Open addressing on symbol indices (0: empty), a power of two at least twice the symbols; for a name defined more
    // than once only the smallest index is kept, the first hit of a linear scan
    std::vector<std::uint32_t> symh_;
};

}  // namespace nxmac::loader
