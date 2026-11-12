#include "nxmac/shader/profiles.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "builtin_profiles.h"

namespace nxmac::shader {
namespace {

bool IsTitleId(std::string_view text) {
    if (text.size() != 16) return false;
    for (const char c : text) {
        if (!absl::ascii_isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

}  // namespace

absl::StatusOr<ShaderProfiles> ShaderProfiles::Parse(std::string_view text) {
    ShaderProfiles out;
    int number = 0;
    for (const std::string_view line : absl::StrSplit(text, '\n')) {
        ++number;
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string_view> f = absl::StrSplit(line, '\t');
        const auto bad = [&](std::string_view why) {
            return absl::InvalidArgumentError(absl::StrFormat("shader profiles, line %d (%s): %s", number, f[0], why));
        };
        if (f[0] == "source") {
            out.source_ = std::string(line.substr(f[0].size() + (f.size() > 1 ? 1 : 0)));
            continue;
        }
        if (f.size() < 2 || !IsTitleId(f[1])) return bad("no title ID");
        const std::string id = absl::AsciiStrToUpper(f[1]);
        if (f[0] == "profile") {
            if (f.size() != 2) return bad("more than a title ID");
            ShaderProfile p;
            p.title_id = id;
            if (!out.profiles_.emplace(id, std::move(p)).second) return bad("a profile listed twice");
            continue;
        }
        const auto it = out.profiles_.find(id);
        if (it == out.profiles_.end()) return bad("a row of a profile that has no profile row before it");
        ShaderProfile& p = it->second;
        if (f[0] == "groups") {
            for (std::size_t i = 2; i < f.size(); ++i) {
                const std::size_t eq = f[i].find('=');
                if (eq == std::string_view::npos) return bad("a group without '='");
                p.groups.emplace_back(std::string(f[i].substr(0, eq)), std::string(f[i].substr(eq + 1)));
            }
        } else if (f[0] == "note") {
            // the rest of the row, tabs included (game.tsv's values joined as dm/games/game.py's get joins them)
            p.note = std::string(line.substr(f[0].size() + 1 + f[1].size() + (f.size() > 2 ? 1 : 0)));
        } else if (f[0] == "key_skip") {
            if (f.size() != 4 || !absl::SimpleAtoi(f[2], &p.key_skip_offset) ||
                !absl::SimpleAtoi(f[3], &p.key_skip_length) || p.key_skip_offset < 0 || p.key_skip_length < 0) {
                return bad("not an offset and a length");
            }
        } else if (f[0] == "modules") {
            p.modules.assign(f.begin() + 2, f.end());
        } else if (f[0] == "fact") {
            if (f.size() != 5 || f[2].empty() || f[3].empty() || f[4].empty())
                return bad("not a key, a name and a value");
            p.facts[std::string(f[2])][std::string(f[3])] = std::string(f[4]);
        } else {
            return bad("an unknown row");
        }
    }
    return out;
}

absl::StatusOr<ShaderProfiles> ShaderProfiles::ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return absl::NotFoundError(absl::StrFormat("%s: %s", path, std::strerror(errno)));
    std::ostringstream text;
    text << in.rdbuf();
    absl::StatusOr<ShaderProfiles> profiles = Parse(text.str());
    if (!profiles.ok())
        return absl::Status(profiles.status().code(), absl::StrCat(path, ": ", profiles.status().message()));
    return profiles;
}

const ShaderProfiles& ShaderProfiles::BuiltIn() {
    static const ShaderProfiles* const profiles = [] {
        absl::StatusOr<ShaderProfiles> parsed = Parse(internal::BuiltInProfilesText());
        // The built-in text is checked when nxmac is built and tested: a table that does not parse is a build error
        return new ShaderProfiles(parsed.ok() ? *std::move(parsed) : ShaderProfiles());
    }();
    return *profiles;
}

absl::StatusOr<const ShaderProfile*> ShaderProfiles::Find(std::string_view title_id) const {
    const auto it = profiles_.find(absl::AsciiStrToUpper(title_id));
    if (it != profiles_.end()) return &it->second;
    std::vector<std::string_view> ids;
    ids.reserve(profiles_.size());
    for (const auto& [id, p] : profiles_) ids.push_back(id);
    return absl::NotFoundError(absl::StrFormat("no shader profile %s (the profiles: %s)", title_id,
                                               ids.empty() ? "none" : absl::StrJoin(ids, ", ")));
}

}  // namespace nxmac::shader
