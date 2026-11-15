#include "modules.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace nxmac::host {
namespace {

std::vector<Module> g_modules;

}  // namespace

std::vector<Module>& Modules() { return g_modules; }

std::uint8_t* GuestSym(std::string_view name) {
    for (const Module& m : g_modules) {
        if (std::uint8_t* const p = m.symbols.Find(name)) return p;
    }
    return nullptr;
}

const Module* ModuleOf(std::uint64_t a) {
    for (const Module& m : g_modules) {
        const auto b = reinterpret_cast<std::uint64_t>(m.image.base());
        if (a >= b && a - b < m.image.size()) return &m;
        if (m.layout.patch_off < 0) {
            const std::uint64_t front = b - static_cast<std::uint64_t>(-m.layout.patch_off);
            if (a >= front && a - front < m.image.patch_len()) return &m;
        }
    }
    return nullptr;
}

const Module* ModuleOf(const void* p) { return ModuleOf(reinterpret_cast<std::uint64_t>(p)); }

std::string SignedHex(std::int64_t v) {
    return absl::StrFormat("%s%#x", v < 0 ? "-" : "",
                           v < 0 ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v));
}

std::string Where(std::uint64_t a) {
    const Module* const m = ModuleOf(a);
    if (m == nullptr) return absl::StrFormat("%#x", a);
    const auto off = static_cast<std::int64_t>(a - reinterpret_cast<std::uint64_t>(m->image.base()));
    return absl::StrCat(m->name, off < 0 ? "" : "+", SignedHex(off));
}

std::string Where(const void* p) { return Where(reinterpret_cast<std::uint64_t>(p)); }

std::string Where(const Module& m, std::uint64_t off) { return absl::StrFormat("%s+%#x", m.name, off); }

}  // namespace nxmac::host
