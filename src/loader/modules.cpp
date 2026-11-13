#include "nxmac/loader/modules.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string_view>

#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"

namespace nxmac::loader {
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// sscanf(line, "# BUILD_ID %32s", bid) == 1: the word after BUILD_ID, at most 32 characters; empty when it does not
// match
std::string BuildIdOf(std::string_view line) {
    if (line.empty() || line.front() != '#') return {};
    line.remove_prefix(1);
    while (!line.empty() && IsSpace(line.front())) line.remove_prefix(1);
    if (!line.starts_with("BUILD_ID")) return {};
    line.remove_prefix(8);
    while (!line.empty() && IsSpace(line.front())) line.remove_prefix(1);
    std::size_t n = 0;
    while (n < line.size() && n < 32 && !IsSpace(line[n])) ++n;
    return std::string(line.substr(0, n));
}

}  // namespace

ModuleChoice ProfileModules(const std::filesystem::path& dir, std::span<const char* const> profile,
                            std::span<const BuildModules> rows) {
    std::string bid;
    if (std::ifstream f(dir / "main.fixups.tsv"); f) {
        for (std::string line; bid.empty() && std::getline(f, line);) bid = BuildIdOf(line);
    }
    ModuleChoice choice;
    for (const BuildModules& row : rows) {
        if (bid.empty() || row.build == nullptr || bid != row.build) continue;
        // In the profile's load order (game.py made the row from it); the row's names are whole words between spaces
        const std::vector<std::string_view> words = absl::StrSplit(row.modules ? row.modules : "", ' ');
        for (const char* name : profile) {
            if (std::find(words.begin(), words.end(), std::string_view(name)) != words.end())
                choice.names.emplace_back(name);
        }
        if (choice.names.size() < profile.size()) {
            choice.note = absl::StrFormat("modules of main's build %.16s: %s (the profile lists %d)", bid, row.modules,
                                          profile.size());
        }
        return choice;
    }
    choice.names.assign(profile.begin(), profile.end());
    return choice;
}

}  // namespace nxmac::loader
