#include "nxmac/loader/symbols.h"

#include <cstring>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace nxmac::loader {
namespace {

// FNV-1a over the name's bytes (sym_hash)
std::uint32_t SymHash(std::string_view s) {
    std::uint32_t h = 2166136261u;
    for (const char c : s) h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
    return h;
}

// strcmp(c, name) == 0 for a NUL-terminated `c`
bool NameIs(const char* c, std::string_view name) {
    return std::strncmp(c, name.data(), name.size()) == 0 && c[name.size()] == '\0';
}

template <typename T>
T Load(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

}  // namespace

absl::StatusOr<ModuleSymbols> ModuleSymbols::Parse(std::uint8_t* base, std::size_t size, std::string_view name) {
    const auto outside = [&](std::string_view what) {
        return absl::InvalidArgumentError(absl::StrFormat("%s: %s is outside the image", name, what));
    };
    if (size < 8) return absl::InvalidArgumentError(absl::StrCat(name, ": no MOD0"));
    const std::uint32_t mod0 = Load<std::uint32_t>(base + 4);
    if (mod0 > size - 8 || std::memcmp(base + mod0, "MOD0", 4) != 0) {
        return absl::InvalidArgumentError(absl::StrCat(name, ": no MOD0"));
    }
    const std::int64_t dyn_off = static_cast<std::int64_t>(mod0) + Load<std::int32_t>(base + mod0 + 4);
    if (dyn_off < 0 || static_cast<std::uint64_t>(dyn_off) > size - sizeof(Elf64Dyn) || dyn_off % 8 != 0) {
        return outside("the dynamic section");
    }

    ModuleSymbols m;
    m.base_ = base;
    m.dyn_ = reinterpret_cast<const Elf64Dyn*>(base + dyn_off);
    std::uint64_t symtab = 0;
    std::uint64_t strtab = 0;
    for (const Elf64Dyn* d = m.dyn_;; ++d) {
        if (reinterpret_cast<const std::uint8_t*>(d + 1) > base + size) return outside("the dynamic section's end");
        if (d->d_tag == 0) break;
        switch (d->d_tag) {
            case kDtSymtab:
                symtab = d->d_val;
                break;
            case kDtStrtab:
                strtab = d->d_val;
                break;
            case kDtInit:
                m.init_ = d->d_val;
                break;
            case kDtFini:
                m.fini_ = d->d_val;
                break;
            case kDtInitArray:
                m.init_array_ = d->d_val;
                break;
            case kDtInitArraySz:
                m.init_arraysz_ = d->d_val;
                break;
            default:
                break;
        }
    }
    // The symbols run up to the string table, which follows them
    if (symtab == 0 || strtab < symtab || strtab >= size || symtab % 8 != 0) return outside("the symbol table");
    m.symtab_ = reinterpret_cast<const Elf64Sym*>(base + symtab);
    m.strtab_ = reinterpret_cast<const char*>(base + strtab);
    m.nsym_ = static_cast<unsigned>((strtab - symtab) / sizeof(Elf64Sym));

    // build_symhash: in ascending index order, a name already there keeps its smaller index
    std::size_t cap = 16;
    while (cap < std::size_t{m.nsym_} * 2) cap <<= 1;
    m.symh_.assign(cap, 0);
    const std::size_t mask = cap - 1;
    for (std::uint32_t i = 1; i < m.nsym_; ++i) {
        const Elf64Sym& s = m.symtab_[i];
        if (s.st_shndx == 0) continue;
        if (s.st_name >= size - strtab) return outside(absl::StrFormat("symbol %u's name", i));
        const char* const nm = m.NameOf(i);
        if (std::memchr(nm, 0, size - strtab - s.st_name) == nullptr) {
            return outside(absl::StrFormat("symbol %u's name's end", i));
        }
        for (std::size_t j = SymHash(nm) & mask;; j = (j + 1) & mask) {
            if (m.symh_[j] == 0) {
                m.symh_[j] = i;
                break;
            }
            if (std::strcmp(m.NameOf(m.symh_[j]), nm) == 0) break;  // the same name with a smaller index: keep that one
        }
    }
    return m;
}

std::uint8_t* ModuleSymbols::Find(std::string_view name) const {
    if (symh_.empty()) return FindLinear(name);
    const std::size_t mask = symh_.size() - 1;
    for (std::size_t j = SymHash(name) & mask; symh_[j] != 0; j = (j + 1) & mask) {
        if (NameIs(NameOf(symh_[j]), name)) return base_ + symtab_[symh_[j]].st_value;
    }
    return nullptr;
}

std::uint8_t* ModuleSymbols::FindLinear(std::string_view name) const {
    for (unsigned i = 1; i < nsym_; ++i) {
        const Elf64Sym& s = symtab_[i];
        if (s.st_shndx != 0 && NameIs(NameOf(i), name)) return base_ + s.st_value;
    }
    return nullptr;
}

}  // namespace nxmac::loader
