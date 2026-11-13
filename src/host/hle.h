// Takeovers by Switch mangled name, as nxnative's dm/host/hle.h. NXHOST_HLE implements an import no module defines;
// NXHOST_HLE_FORCE takes a name over wherever a module defines it (the SDK's service clients), and says why. Each entry
// goes into a section of nxhost's own image (__DATA,swhle / __DATA,swforce), so a file adds its takeovers by being
// compiled into nxhost; the loader reads the sections back (HleTable, ForceTable) and binds the modules to them.
//
// Guest code calls these functions. The Switch's calling convention differs from Apple's AAPCS64 in two ways: the
// caller does not extend arguments narrower than 32 bits (so such parameters are declared int / unsigned and truncated
// here), and variadic arguments go in registers (so never a "..." parameter). Nothing may throw out of them.
#pragma once

#include <span>

#include "nxmac/loader/hle.h"

#define NXHOST_HLE(mangled, ret, cname, ...)                                                                  \
    static ret cname(__VA_ARGS__) noexcept;                                                                   \
    __attribute__((used, section("__DATA,swhle"))) static const ::nxmac::loader::HleEntry hle_ent_##cname = { \
        mangled, reinterpret_cast<const void*>(&cname)};                                                      \
    static ret cname(__VA_ARGS__) noexcept

#define NXHOST_HLE_FORCE(mangled, why, ret, cname, ...)                                                                \
    static ret cname(__VA_ARGS__) noexcept;                                                                            \
    __attribute__((used, section("__DATA,swforce"))) static const ::nxmac::loader::HleForceEntry hle_force_##cname = { \
        mangled, reinterpret_cast<const void*>(&cname), why};                                                          \
    static ret cname(__VA_ARGS__) noexcept

namespace nxmac::host {

// The entries every file compiled into nxhost put in the sections (empty when none did)
std::span<const loader::HleEntry> HleTable();
std::span<const loader::HleForceEntry> ForceTable();

}  // namespace nxmac::host
