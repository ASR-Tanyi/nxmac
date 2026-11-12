#include <algorithm>
#include <thread>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "nxmac/shader/profiles.h"
#include "nxmac/shader/split.h"
#include "shader/options.h"
#include "shader/shader.h"

namespace nxmac::shader_tool {
namespace {

int Stop(std::ostream& err, const absl::Status& status) {
    err << "[RED] " << status.message() << "\n";
    return kFailed;
}

int Profile(const shader::ShaderProfiles& profiles, const Options& options, std::ostream& out, std::ostream& err) {
    int code = kOk;
    for (const std::string& id : options.romfs) {
        absl::StatusOr<const shader::ShaderProfile*> p = profiles.Find(id);
        if (!p.ok()) {
            err << "shader: " << p.status().message() << "\n";
            code = kFailed;
            continue;
        }
        const shader::ShaderProfile& g = **p;
        std::vector<std::string> groups;
        groups.reserve(g.groups.size());
        for (const auto& [part, group] : g.groups) groups.push_back(absl::StrCat(part, "=", group));
        out << g.title_id << "\n"
            << "  note      " << g.note << "\n"
            << "  groups    " << absl::StrJoin(groups, " ") << "\n"
            << "  key skip  " << g.key_skip_offset << "+" << g.key_skip_length << "\n"
            << "  modules   " << absl::StrJoin(g.modules, " ") << "\n";
        for (const auto& [key, members] : g.facts) {
            std::vector<std::string> parts;
            parts.reserve(members.size());
            for (const auto& [name, value] : members) parts.push_back(absl::StrCat("\"", name, "\":", value));
            out << "  fact      " << key << " {" << absl::StrJoin(parts, ",") << "}\n";
        }
    }
    return code;
}

int Split(const shader::ShaderProfiles& profiles, const Options& options, std::ostream& out, std::ostream& err) {
    absl::StatusOr<const shader::ShaderProfile*> profile = profiles.Find(options.game);
    if (!profile.ok()) return Stop(err, profile.status());
    shader::SplitOptions split;
    split.profile = *profile;
    for (std::size_t i = 0; i < options.romfs.size(); ++i)
        split.versions.push_back({options.romfs[i], options.mains[i]});
    split.dlc = options.dlc;
    split.jobs = options.jobs ? options.jobs : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    split.glslc_capture = options.glslc_capture;
    split.print = [&out](const std::string& line) { out << line << "\n" << std::flush; };
    absl::StatusOr<shader::SplitResult> result = shader::Split(split);
    if (!result.ok()) return Stop(err, result.status());
    if (absl::Status s = shader::WriteSplit(*result, options.out); !s.ok()) return Stop(err, s);
    return kOk;
}

}  // namespace

int RunShader(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    absl::StatusOr<Options> options = ParseOptions(args);
    if (!options.ok()) {
        err << "shader: " << options.status().message() << "\n\n" << Usage();
        return kUsage;
    }
    if (options->command == "help") {
        out << Usage();
        return kOk;
    }
    const shader::ShaderProfiles* profiles = &shader::ShaderProfiles::BuiltIn();
    absl::StatusOr<shader::ShaderProfiles> from_file;
    if (!options->profiles.empty()) {
        from_file = shader::ShaderProfiles::ReadFile(options->profiles);
        if (!from_file.ok()) {
            err << "shader: " << from_file.status().message() << "\n";
            return kFailed;
        }
        profiles = &*from_file;
    }
    if (options->command == "profile") return Profile(*profiles, *options, out, err);
    return Split(*profiles, *options, out, err);
}

}  // namespace nxmac::shader_tool
