// Which modules nxhost loads when its command line names none: those of the profile's build row for main's build (its
// build ID from main's layout table), else all the profile's modules. A game's versions may differ in modules (Mario
// Kart World's base game has main subsdk0 sdk, its update 1.8.0 main subsdk0 subsdk1 sdk). A port of nxnative's
// dm/host/main.c profile_modules.
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace nxmac::loader {

// A row of the profile's build_modules (game_profile.h DM_GAME_BUILD_MODULES): main's build ID and that build's
// modules, separated by spaces. A row without a build ID ({0}: a profile without rows) matches none.
struct BuildModules {
    const char* build = nullptr;
    const char* modules = nullptr;
};

struct ModuleChoice {
    std::vector<std::string> names;  // in the profile's load order
    std::string note;  // the line nxnative's host logs when main's build has fewer modules than the profile; else empty
};

// `profile` is the profile's modules in load order (DM_GAME_MODULES); main's build ID comes from <dir>/main.fixups.tsv,
// and no table (or no BUILD_ID line in it) means all of them.
ModuleChoice ProfileModules(const std::filesystem::path& dir, std::span<const char* const> profile,
                            std::span<const BuildModules> rows);

}  // namespace nxmac::loader
