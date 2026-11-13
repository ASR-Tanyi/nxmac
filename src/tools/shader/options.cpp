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

usage: shader <command> [options] [<RomFS>... | <package folder>]

commands:
  split -g <id> -o <dir> --main <main> <RomFS>
                        every shader of the game's RomFS (a folder, or the DMROMFS1 image nsp extract writes) and of
                        its ExeFS main, into what nxnative's dmsh_build.py and host read: <dir>/pkg/*.bin (one
                        record per shader key) and the tables <dir>/shader_map.tsv, shader_facts.tsv,
                        shader_pairs.tsv and pso_pairs.tsv, byte for byte what shader_split.py writes. Several
                        versions (base game first, each RomFS with its --main in the same order) give their union
  build -o <dir> (-g <id> --nxnative <dir> | --profile <dir> --xlate <file>) <package folder>
                        the game's shader library for nxnative's host from the split's package folder (<dir>/pkg
                        above): each shader through nxnative's translator, SPIRV-Cross and the rewrites of
                        nxnative's dmsh_build.py to Metal source, its reflection and its libraries (Apple's Metal
                        compiler, from Xcode 26), all in <dir>/b-<chain>.img with the stamp inputs.tsv; and the
                        tables c1_full.tsv and cb_extent.tsv (in --tables). nxnative's dmsh_build.py, rewritten;
                        vertex, pixel and compute shaders of NVN 1 (Breath of the Wild's) are ported
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
  -j, --jobs <n>           files scanned, or shaders built, at once (default: one per CPU)

build options:
      --nxnative <dir>     nxnative's checkout: the profile folder dm/games/<id> (-g) and the translator
                           dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate, unless given on their own
      --profile <dir>      the game's profile folder: game.tsv and the shader tables (shader_map.tsv, ...)
      --xlate <file>       nxnative's translator, dmsh_xlate
      --tables <dir>       where c1_full.tsv and cb_extent.tsv go (default: -o; nxnative's host reads them from
                           the profile folder)
      --cache <dir>        the shaders and Metal libraries kept for the next build (default: <dir>/cache)
      --min-macos <ver>    the libraries' minimum macOS (default 15.0; none: the SDK's own, as nxnative compiles)

A line that starts with [RED] reports something wrong or not done: a file left out (one that does not read, or one in
another engine's format whose reader is not ported to nxmac yet), or why the split stopped.

exit codes: 0 done (files left out, or shaders that do not build, are reported, not failures); 1 the split or build
stopped (the split writes nothing, the build no library); 2 a wrong command line
)";
}

absl::StatusOr<Options> ParseOptions(const std::vector<std::string>& args) {
    Options options;
    if (args.empty()) return Invalid("no command");
    options.command = args[0];
    if (options.command == "-h" || options.command == "--help") options.command = "help";
    if (options.command != "split" && options.command != "build" && options.command != "profile" &&
        options.command != "help") {
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
        } else if (name == "--nxnative") {
            status = take(&options.nxnative);
        } else if (name == "--profile") {
            status = take(&options.profile);
        } else if (name == "--xlate") {
            status = take(&options.xlate);
        } else if (name == "--tables") {
            status = take(&options.tables);
        } else if (name == "--cache") {
            status = take(&options.cache);
        } else if (name == "--min-macos") {
            status = take(&options.min_macos);
            if (status.ok() && options.min_macos == "none") options.min_macos.clear();
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
        if (!options.nxnative.empty() || !options.profile.empty() || !options.xlate.empty() ||
            !options.tables.empty() || !options.cache.empty()) {
            return Invalid("profile takes only -p among the options");
        }
        if (!options.out.empty() || !options.game.empty() || !options.mains.empty() || !options.dlc.empty() ||
            !options.glslc_capture.empty() || options.jobs != 0) {
            return Invalid("profile takes only -p among the options");
        }
        return options;
    }
    if (c == "build") {
        if (options.out.empty()) return Invalid("build needs -o <dir>");
        if (options.romfs.size() != 1) return Invalid("build takes one package folder (the split's <dir>/pkg)");
        if (!options.mains.empty() || !options.dlc.empty() || !options.glslc_capture.empty() ||
            !options.profiles.empty())
            return Invalid("build takes no --main, --dlc, --glslc-capture or -p (its profile folder is --profile)");
        if (options.profile.empty() && (options.nxnative.empty() || options.game.empty()))
            return Invalid("build needs the game's profile folder: --profile <dir>, or -g <id> with --nxnative <dir>");
        if (options.xlate.empty() && options.nxnative.empty())
            return Invalid("build needs nxnative's translator: --xlate <file>, or --nxnative <dir>");
        return options;
    }
    if (!options.nxnative.empty() || !options.profile.empty() || !options.xlate.empty() || !options.tables.empty() ||
        !options.cache.empty())
        return Invalid("split takes no build options (--nxnative, --profile, --xlate, --tables, --cache)");
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
