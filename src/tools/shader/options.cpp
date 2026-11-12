#include "shader/options.h"

#include <charconv>
#include <optional>
#include <string_view>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

namespace nxmac::shader_tool {
namespace {

absl::Status Invalid(std::string_view message) { return absl::InvalidArgumentError(message); }

}  // namespace

std::string Usage() {
    return R"(shader: a game's NVN shaders, split out for nxnative's shader build (nxmac's rewrite of nxnative's shader_split.py)

usage: shader <command> [options] [<RomFS>...]

commands:
  split -g <id> -o <dir> --main <main> <RomFS>
                        every shader of the game's RomFS (a folder, or the DMROMFS1 image nsp extract writes) and of
                        its ExeFS main, into what nxnative's dmsh_build.py and host read: <dir>/pkg/*.bin (one
                        record per shader key) and the tables <dir>/shader_map.tsv, shader_facts.tsv,
                        shader_pairs.tsv and pso_pairs.tsv, byte for byte what shader_split.py writes. Several
                        versions (base game first, each RomFS with its --main in the same order) give their union
  profile <id>...       what the profile table holds for these games (groups, note, key skip, hand-written facts)
  help                  this text

options:
  -g, --game <id>          the game's profile, its base title ID (01007EF00011E000; nxnative's DM_GAME)
  -o, --out <dir>          where split writes
      --main <file>        a version's ExeFS main, once per RomFS
      --dlc <RomFS>        an add-on content RomFS (as many as there are)
      --glslc-capture <dir>  GLSLC outputs captured on the Mac (nxnative's DM_GLSLC_CAPTURE; default: glslc_capture
                           beside each RomFS)
  -p, --profiles <file>    a profile table instead of the built-in one (tools/export_shader_profiles.py writes it)
  -j, --jobs <n>           files scanned at once (default: one per CPU)

A line that starts with [RED] reports something wrong or not done: a file left out (one that does not read, or one in
another engine's format whose reader is not ported to nxmac yet), or why the split stopped.

exit codes: 0 done (files left out are reported, not failures); 1 the split stopped (nothing is written); 2 a wrong
command line
)";
}

absl::StatusOr<Options> ParseOptions(const std::vector<std::string>& args) {
    Options options;
    if (args.empty()) return Invalid("no command");
    options.command = args[0];
    if (options.command == "-h" || options.command == "--help") options.command = "help";
    if (options.command != "split" && options.command != "profile" && options.command != "help") {
        return Invalid(absl::StrCat("unknown command \"", options.command, "\""));
    }
    bool only_paths = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (only_paths || arg.empty() || arg[0] != '-' || arg == "-") {
            options.romfs.push_back(arg);
            continue;
        }
        if (arg == "--") {
            only_paths = true;
            continue;
        }
        // An option's value: "--name=value", or the next argument
        std::string name = arg;
        std::optional<std::string> value;
        if (const std::size_t eq = arg.find('='); absl::StartsWith(arg, "--") && eq != std::string::npos) {
            name = arg.substr(0, eq);
            value = arg.substr(eq + 1);
        }
        const auto take = [&](std::string* into) -> absl::Status {
            if (value) {
                *into = *value;
            } else if (i + 1 >= args.size()) {
                return Invalid(absl::StrCat(name, " needs a value"));
            } else {
                *into = args[++i];
            }
            if (into->empty()) return Invalid(absl::StrCat(name, " needs a value"));
            return absl::OkStatus();
        };
        const auto add = [&](std::vector<std::string>* into) -> absl::Status {
            std::string v;
            absl::Status s = take(&v);
            if (s.ok()) into->push_back(std::move(v));
            return s;
        };
        absl::Status status;
        if (name == "-h" || name == "--help") {
            options.command = "help";
        } else if (name == "-o" || name == "--out") {
            status = take(&options.out);
        } else if (name == "-g" || name == "--game") {
            status = take(&options.game);
        } else if (name == "-p" || name == "--profiles") {
            status = take(&options.profiles);
        } else if (name == "--main") {
            status = add(&options.mains);
        } else if (name == "--dlc") {
            status = add(&options.dlc);
        } else if (name == "--glslc-capture") {
            status = take(&options.glslc_capture);
        } else if (name == "-j" || name == "--jobs") {
            std::string v;
            status = take(&v);
            if (status.ok()) {
                const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), options.jobs);
                if (ec != std::errc() || end != v.data() + v.size() || options.jobs < 1 || options.jobs > 256) {
                    return Invalid(absl::StrCat(name, " takes a number of files from 1 to 256, not \"", v, "\""));
                }
            }
        } else {
            return Invalid(absl::StrCat("unknown option ", arg));
        }
        if (!status.ok()) return status;
    }
    const std::string& c = options.command;
    if (c == "help") return options;
    if (c == "profile") {
        if (options.romfs.empty()) return Invalid("profile needs at least one title ID");
        if (!options.out.empty() || !options.game.empty() || !options.mains.empty() || !options.dlc.empty() ||
            !options.glslc_capture.empty() || options.jobs != 0) {
            return Invalid("profile takes only -p among the options");
        }
        return options;
    }
    if (options.game.empty()) return Invalid("split needs -g <base title ID>");
    if (options.out.empty()) return Invalid("split needs -o <dir>");
    if (options.romfs.empty()) return Invalid("split needs a RomFS (a folder or a DMROMFS1 image)");
    if (options.mains.size() != options.romfs.size()) {
        return Invalid(absl::StrCat(options.romfs.size(), " RomFS and ", options.mains.size(),
                                    " --main: each RomFS takes the main of its version, in the same order"));
    }
    return options;
}

}  // namespace nxmac::shader_tool
