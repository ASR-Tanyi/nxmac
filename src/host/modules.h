// nxhost's loaded modules (nxnative's dm.h module_t and g_mod): each one's layout table, mapped image and symbols, in
// load order, and the lookups the rest of the host makes on them.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nxmac/loader/image.h"
#include "nxmac/loader/layout.h"
#include "nxmac/loader/symbols.h"

namespace nxmac::host {

struct Module {
    std::string name;
    loader::ModuleLayout layout;
    loader::ModuleImage image;
    loader::ModuleSymbols symbols;
};

// The modules, in load order; filled once, before any guest code runs
std::vector<Module>& Modules();

// The first module, in load order, that defines `name` (loader.c guest_sym / loaded_sym); null when none does
std::uint8_t* GuestSym(std::string_view name);

// The module `p` is in, a far module's front patch area included (loader.c module_of); null when it is in none
const Module* ModuleOf(const void* p);
const Module* ModuleOf(std::uint64_t addr);

// "main+0x12afc" for an address in a module, else the address
std::string Where(const void* p);
std::string Where(std::uint64_t addr);
// The same for a module's own offset: "sdk+0x1430"
std::string Where(const Module& m, std::uint64_t off);

// A signed offset in hex: -0x2000000 for a patch area before the base
std::string SignedHex(std::int64_t v);

}  // namespace nxmac::host
