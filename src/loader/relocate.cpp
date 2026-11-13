#include "nxmac/loader/relocate.h"

#include <cstring>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace nxmac::loader {
namespace {

constexpr int kStbLocal = 0;
constexpr int kStbWeak = 2;
constexpr int kStvDefault = 0;

int Bind(const Elf64Sym& s) { return s.st_info >> 4; }
int Visibility(const Elf64Sym& s) { return s.st_other & 3; }

// The first loaded module, in load order, that defines `name` (loaded_sym); 0 when none does
std::uint64_t LoadedSym(std::span<const LoadedModule> modules, std::string_view name) {
    for (const LoadedModule& m : modules) {
        if (std::uint8_t* const p = m.symbols->Find(name)) return reinterpret_cast<std::uint64_t>(p);
    }
    return 0;
}

// The module an address is in, a far module's front patch area included (module_of)
const LoadedModule* ModuleOf(std::span<const LoadedModule> modules, std::uint64_t v) {
    for (const LoadedModule& m : modules) {
        const auto b = reinterpret_cast<std::uint64_t>(m.base);
        if (v >= b && v < b + m.size) return &m;
        if (m.patch_off < 0 && v >= b + m.patch_off && v < b + m.patch_off + m.patch_len) return &m;
    }
    return nullptr;
}

// The relocation tables the dynamic section names (relocs_of)
struct Relocs {
    std::uint64_t tab[2] = {};  // DT_RELA, DT_JMPREL
    std::uint64_t n[2] = {};
    std::uint64_t relr = 0;
    std::uint64_t relrsz = 0;
};

Relocs RelocsOf(const ModuleSymbols& symbols) {
    std::uint64_t relasz = 0;
    std::uint64_t pltsz = 0;
    Relocs r;
    for (const Elf64Dyn* d = symbols.dyn(); d->d_tag != 0; ++d) {
        switch (d->d_tag) {
            case kDtRela:
                r.tab[0] = d->d_val;
                break;
            case kDtRelaSz:
                relasz = d->d_val;
                break;
            case kDtJmpRel:
                r.tab[1] = d->d_val;
                break;
            case kDtPltRelSz:
                pltsz = d->d_val;
                break;
            case kDtRelr:
                r.relr = d->d_val;
                break;
            case kDtRelrSz:
                r.relrsz = d->d_val;
                break;
            default:
                break;
        }
    }
    r.n[0] = relasz / sizeof(Elf64Rela);
    r.n[1] = pltsz / sizeof(Elf64Rela);
    return r;
}

bool Inside(const LoadedModule& m, std::uint64_t off, std::uint64_t len) {
    return off <= m.size && len <= m.size - off;
}

void Add(std::uint8_t* where, std::uint64_t delta) {
    std::uint64_t v;
    std::memcpy(&v, where, 8);
    v += delta;
    std::memcpy(where, &v, 8);
}

// DT_RELR (relr_apply): each 8-byte entry is either a new start offset (even: relocate it, then the next slot) or a
// bitmap (odd) whose bits 1-63 stand for the 63 slots from there, which then moves on by 63 slots
absl::StatusOr<unsigned> RelrApply(const LoadedModule& m, std::uint64_t relr, std::uint64_t relrsz) {
    if (!Inside(m, relr, relrsz))
        return absl::InvalidArgumentError(absl::StrCat(m.name, ": DT_RELR is outside the image"));
    const auto delta = reinterpret_cast<std::uint64_t>(m.base);
    std::uint64_t where = 0;  // the next slot's offset
    bool started = false;
    unsigned n = 0;
    for (std::uint64_t i = 0; i < relrsz / 8; ++i) {
        std::uint64_t v;
        std::memcpy(&v, m.base + relr + i * 8, 8);
        if ((v & 1) == 0) {
            if (!Inside(m, v, 8)) {
                return absl::InvalidArgumentError(
                    absl::StrFormat("%s: DT_RELR entry %#x is outside the image", m.name, v));
            }
            Add(m.base + v, delta);
            where = v + 8;
            started = true;
            ++n;
            continue;
        }
        if (!started) {
            return absl::InvalidArgumentError(absl::StrCat(m.name, ": DT_RELR bitmap entry with no start before it"));
        }
        for (unsigned b = 0; (v >>= 1) != 0; ++b) {
            if ((v & 1) == 0) continue;
            if (!Inside(m, where + b * 8, 8)) {
                return absl::InvalidArgumentError(
                    absl::StrFormat("%s: DT_RELR slot %#x is outside the image", m.name, where + b * 8));
            }
            Add(m.base + where + b * 8, delta);
            ++n;
        }
        where += 63 * 8;
    }
    return n;
}

}  // namespace

absl::StatusOr<RelocationReport> Relocate(std::span<const LoadedModule> modules, std::size_t index,
                                          ImportResolver& resolver) {
    const LoadedModule& m = modules[index];
    const ModuleSymbols& syms = *m.symbols;
    const Relocs rl = RelocsOf(syms);
    RelocationReport report;
    if (rl.relr != 0) {
        absl::StatusOr<unsigned> n = RelrApply(m, rl.relr, rl.relrsz);
        if (!n.ok()) return n.status();
        report.relr = *n;
    }
    // resolve_sym: a loaded module's definition, else the host's
    const auto resolve_sym = [&](std::string_view name) -> absl::StatusOr<std::uint64_t> {
        const std::uint64_t p = LoadedSym(modules, name);
        if (p != 0) return p;
        return resolver.Import(m.name, name);
    };
    for (int t = 0; t < 2; ++t) {
        if (!Inside(m, rl.tab[t], rl.n[t] * sizeof(Elf64Rela))) {
            return absl::InvalidArgumentError(
                absl::StrCat(m.name, t == 0 ? ": DT_RELA" : ": DT_JMPREL", " is outside the image"));
        }
        for (std::uint64_t i = 0; i < rl.n[t]; ++i) {
            Elf64Rela r;
            std::memcpy(&r, m.base + rl.tab[t] + i * sizeof r, sizeof r);
            if (!Inside(m, r.r_offset, 8)) {
                return absl::InvalidArgumentError(
                    absl::StrFormat("%s: relocation at %#x is outside the image", m.name, r.r_offset));
            }
            std::uint8_t* const where = m.base + r.r_offset;
            const auto type = static_cast<std::uint32_t>(r.r_info & 0xffffffff);
            const auto si = static_cast<std::uint32_t>(r.r_info >> 32);
            if (type == kRAarch64Relative) {
                const std::uint64_t v =
                    reinterpret_cast<std::uint64_t>(m.base) + static_cast<std::uint64_t>(r.r_addend);
                std::memcpy(where, &v, 8);
                ++report.relative;
                continue;
            }
            if (si >= syms.nsym()) {
                return absl::InvalidArgumentError(absl::StrFormat("%s: relocation at %#x names symbol %u of %u", m.name,
                                                                  r.r_offset, si, syms.nsym()));
            }
            const Elf64Sym& s = syms.symtab()[si];
            const std::string_view nm = syms.strtab() + s.st_name;

            // bind_sym
            std::uint64_t v = resolver.Forced(nm);
            bool other = false;
            if (v == 0 && s.st_shndx != 0) {
                const std::uint64_t self = reinterpret_cast<std::uint64_t>(m.base) + s.st_value;
                if (Bind(s) == kStbLocal || Visibility(s) != kStvDefault) {
                    v = self;
                } else {
                    const absl::StatusOr<std::uint64_t> p = resolve_sym(nm);
                    if (!p.ok()) return p.status();
                    v = *p;
                }
                if (v == 0) v = self;
                other = v != self;
            }
            const bool weak0 = v == 0 && s.st_shndx == 0 && Bind(s) == kStbWeak && LoadedSym(modules, nm) == 0 &&
                               !resolver.Implements(nm);
            if (v == 0 && !weak0) {
                const absl::StatusOr<std::uint64_t> p = resolve_sym(nm);
                if (!p.ok()) return p.status();
                v = *p;
            }

            if (other && report.other++ < 8) {
                const LoadedModule* const owner = ModuleOf(modules, v);
                report.lines.push_back(absl::StrFormat("%s: %s bound to an earlier module's definition (%s)", m.name,
                                                       nm, owner != nullptr ? owner->name : "?"));
            }
            if (weak0 && report.weak0++ < 8) {
                report.lines.push_back(absl::StrFormat("%s: undefined weak %s bound to 0", m.name, nm));
            }
            if (type == kRAarch64JumpSlot) {
                std::memcpy(where, &v, 8);
                ++report.jump;
            } else {  // ABS64 / GLOB_DAT
                const std::uint64_t w = v + static_cast<std::uint64_t>(r.r_addend);
                std::memcpy(where, &w, 8);
                ++report.abs_glob;
            }
        }
    }
    report.lines.push_back(absl::StrFormat("%s: reloc relative=%d relr=%u abs/glob=%d jump=%d%s", m.name,
                                           report.relative, report.relr, report.abs_glob, report.jump,
                                           report.other != 0 ? " (some bound to an earlier module's definition)" : ""));
    if (report.other > 8) {
        report.lines.push_back(
            absl::StrFormat("%s: %d bound to an earlier module's definition in all", m.name, report.other));
    }
    return report;
}

}  // namespace nxmac::loader
