#include "nxmac/rewriter/patches.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "builtin_patches.h"
#include "text.h"

namespace nxmac::rewriter {
namespace {

using internal::HexBytes;
using internal::ParseInt;

bool IsNumber(std::string_view text) {
    if (text.empty()) return false;
    const bool hex = text.size() > 2 && text[0] == '0' && text[1] == 'x';
    for (const char c : text.substr(hex ? 2 : 0)) {
        const bool digit = (c >= '0' && c <= '9') || (hex && c >= 'a' && c <= 'f');
        if (!digit) return false;
    }
    return true;
}

bool IsHex(std::string_view text, std::size_t digits) {
    if (text.size() != digits) return false;
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

template <typename Map>
auto Find(const Map& map, std::string_view key) -> const typename Map::mapped_type* {
    const auto it = map.find(key);
    return it == map.end() ? nullptr : &it->second;
}

}  // namespace

absl::StatusOr<PatchTables> PatchTables::Parse(std::string_view text) {
    PatchTables tables;
    MatchRule* rule = nullptr;
    int number = 0;
    for (const std::string_view line : absl::StrSplit(text, '\n')) {
        ++number;
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string_view> f = absl::StrSplit(line, '\t');
        const auto bad = [&](std::string_view why) {
            return absl::InvalidArgumentError(absl::StrFormat("patch tables, line %d (%s): %s", number, f[0], why));
        };
        // A cut's bytes: whole words of hex digits, as many new as original
        const auto cut = [&](std::size_t at, Cut* out) {
            if (!IsNumber(f[at])) return false;
            const auto original = HexBytes(f[at + 1]);
            const auto replacement = HexBytes(f[at + 2]);
            if (!original || !replacement || original->empty() || original->size() != replacement->size()) return false;
            *out = {ParseInt(f[at]), std::string(f[at + 1]), std::string(f[at + 2]), std::string(f[at + 3])};
            return true;
        };
        const std::string_view kind = f[0];
        if (kind != "function" && kind != "fcut") rule = nullptr;  // only they go on with the last match row
        if (kind == "source" && f.size() == 2) {
            tables.source_ = std::string(f[1]);
        } else if (kind == "cuts" && f.size() == 2 && IsHex(f[1], 32)) {
            tables.cuts_[std::string(f[1])];
        } else if (kind == "cut" && f.size() == 6 && IsHex(f[1], 32)) {
            Cut c;
            if (!cut(2, &c)) return bad("an offset, then original and new bytes of one length");
            tables.cuts_[std::string(f[1])].push_back(std::move(c));
        } else if (kind == "obs" && f.size() == 6 && IsHex(f[1], 32)) {
            if (!IsNumber(f[2]) || !IsNumber(f[3]) || !IsHex(f[4], 8)) return bad("offset, number, instruction word");
            tables.obs_[std::string(f[1])].push_back(
                {ParseInt(f[2]), static_cast<std::uint32_t>(ParseInt(f[3])), std::string(f[4]), std::string(f[5])});
        } else if (kind == "capability" && f.size() == 3 && IsHex(f[1], 32)) {
            tables.capabilities_[std::string(f[1])].emplace_back(f[2]);
        } else if (kind == "frame_step" && f.size() == 3 && IsHex(f[1], 32)) {
            tables.frame_step_[std::string(f[1])] = std::string(f[2]);
        } else if (kind == "stubs" && f.size() == 3 && IsHex(f[1], 32)) {
            tables.stubs_[std::string(f[1])] = std::string(f[2]);
        } else if (kind == "match" && f.size() == 5 && (f[3] == "one" || f[3] == "several")) {
            std::vector<MatchRule>& rules = tables.match_[absl::StrCat(f[1], "\t", f[2])];
            rule = &rules.emplace_back();
            rule->name = std::string(f[4]);
            rule->several = f[3] == "several";
        } else if (kind == "function" && f.size() == 3 && rule != nullptr) {
            if (!IsNumber(f[1]) || ParseInt(f[1]) <= 0 || ParseInt(f[1]) % 4 != 0) return bad("a function size");
            FunctionRule& fn = rule->functions.emplace_back();
            fn.size = ParseInt(f[1]);
            for (const std::string_view digest : absl::StrSplit(f[2], ',')) {
                if (!IsHex(digest, 64)) return bad("fingerprints of 64 hex digits");
                fn.fingerprints.emplace_back(digest);
            }
        } else if (kind == "fcut" && f.size() == 5 && rule != nullptr && !rule->functions.empty()) {
            Cut c;
            if (!cut(1, &c)) return bad("an offset, then original and new bytes of one length");
            rule->functions.back().cuts.push_back(std::move(c));
        } else if (kind == "match_frame_step" && f.size() == 4) {
            tables.match_frame_step_[absl::StrCat(f[1], "\t", f[2])].emplace_back(f[3]);
        } else if (kind == "build" && f.size() >= 4) {
            ProfileBuild& build = tables.builds_[std::string(f[1])].emplace_back();
            build.version = std::string(f[2]);
            build.display = std::string(f[3]);
            for (std::size_t i = 4; i < f.size(); ++i) {
                const std::size_t eq = f[i].find('=');
                if (eq == std::string_view::npos) continue;
                build.modules.emplace_back(f[i].substr(0, eq), f[i].substr(eq + 1));
            }
        } else {
            return bad("not a row of the patch tables");
        }
    }
    for (const auto& [key, rules] : tables.match_) {
        for (const MatchRule& r : rules) {
            const bool whole = !r.functions.empty() && (r.several ? r.functions.size() >= 2 : r.functions.size() == 1);
            bool cuts = whole;
            for (const FunctionRule& fn : r.functions) cuts = cuts && !fn.cuts.empty();
            if (!cuts) {
                return absl::InvalidArgumentError(
                    absl::StrFormat("patch tables: the function patch '%s' has no functions or cuts", r.name));
            }
        }
    }
    return tables;
}

absl::StatusOr<PatchTables> PatchTables::ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return absl::NotFoundError(absl::StrFormat("%s: %s", path, std::strerror(errno)));
    std::ostringstream text;
    text << in.rdbuf();
    absl::StatusOr<PatchTables> tables = Parse(text.str());
    if (!tables.ok()) return absl::Status(tables.status().code(), absl::StrCat(path, ": ", tables.status().message()));
    return tables;
}

const PatchTables& PatchTables::BuiltIn() {
    static const PatchTables* const tables = [] {
        absl::StatusOr<PatchTables> parsed = Parse(internal::BuiltInPatchesText());
        // The built-in text is checked when nxmac is built and tested: a table that does not parse is a build error
        return new PatchTables(parsed.ok() ? *std::move(parsed) : PatchTables());
    }();
    return *tables;
}

const std::vector<Cut>* PatchTables::Cuts(std::string_view build_id) const { return Find(cuts_, build_id); }
const std::vector<ObservationGate>* PatchTables::Obs(std::string_view build_id) const { return Find(obs_, build_id); }
const std::vector<std::string>* PatchTables::Capabilities(std::string_view build_id) const {
    return Find(capabilities_, build_id);
}
const std::string* PatchTables::FrameStep(std::string_view build_id) const { return Find(frame_step_, build_id); }
const std::string* PatchTables::Stubs(std::string_view build_id) const { return Find(stubs_, build_id); }
const std::vector<MatchRule>* PatchTables::ProfileMatchCuts(std::string_view profile, std::string_view module) const {
    return Find(match_, absl::StrCat(profile, "\t", module));
}
const std::vector<MatchRule>* PatchTables::SharedMatchCuts(std::string_view module) const {
    return Find(match_, absl::StrCat("*\t", module));
}
const std::vector<std::string>* PatchTables::MatchFrameStep(std::string_view profile, std::string_view module) const {
    return Find(match_frame_step_, absl::StrCat(profile, "\t", module));
}
const std::vector<ProfileBuild>* PatchTables::Builds(std::string_view profile) const { return Find(builds_, profile); }

}  // namespace nxmac::rewriter
