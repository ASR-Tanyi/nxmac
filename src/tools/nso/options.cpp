#include "nso/options.h"

#include <charconv>
#include <optional>
#include <string_view>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

namespace nxmac::nso {
namespace {

absl::Status Invalid(std::string_view message) { return absl::InvalidArgumentError(message); }

}  // namespace

std::string Usage() {
    return R"(nso: Switch code modules, rewritten to run natively on Apple systems (nxmac's rewrite of nxnative's patch_nso.py)

usage: nso <command> [options] <module>...

commands:
  rewrite <module>... -o <dir>  rewrite the modules of a game's ExeFS (main, sdk, subsdk0, ...) where Apple's kernel
                                requires it. For each, <dir> gets what nxnative's host loads: <name>.patched.bin, and
                                the tables <name>.fixups.tsv, .gates.tsv, .sites.tsv, .cuts.tsv, .obs.tsv and
                                .changed.tsv (and .match.tsv for a build the patch tables do not register). The
                                module's name is its file's name
  info <module>...              what each module is (segments, build ID) and what the patch tables hold for its build
  help                          this text

options:
  -o, --out <dir>       where rewrite writes
  -g, --game <id>       the game's profile, its base title ID (01007EF00011E000): the profile's function-matched
                        patches apply to builds it does not register (nxnative's DM_GAME)
  -n, --name <name>     the module's name, when its file is not named after it (one module)
  -p, --patches <file>  patch tables to use instead of the built-in ones (tools/export_patches.py writes them)
  -j, --jobs <n>        modules rewritten at once (default: one per CPU)
      --spin-list       a line for each spin wait found

A line that starts with [RED] reports something the rewrite could not do as nxnative's would.

exit codes: 0 done; 1 an error (nothing is written for a module that cannot be rewritten); 2 a wrong command line
)";
}

absl::StatusOr<Options> ParseOptions(const std::vector<std::string>& args) {
    Options options;
    if (args.empty()) return Invalid("no command");
    options.command = args[0];
    if (options.command == "-h" || options.command == "--help") options.command = "help";
    if (options.command != "rewrite" && options.command != "info" && options.command != "help") {
        return Invalid(absl::StrCat("unknown command \"", options.command, "\""));
    }
    bool only_modules = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (only_modules || arg.empty() || arg[0] != '-' || arg == "-") {
            options.modules.push_back(arg);
            continue;
        }
        if (arg == "--") {
            only_modules = true;
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
            return absl::OkStatus();
        };
        absl::Status status;
        if (name == "-h" || name == "--help") {
            options.command = "help";
        } else if (name == "-o" || name == "--out") {
            status = take(&options.out);
        } else if (name == "-g" || name == "--game") {
            status = take(&options.game);
        } else if (name == "-n" || name == "--name") {
            status = take(&options.name);
        } else if (name == "-p" || name == "--patches") {
            status = take(&options.patches);
        } else if (name == "-j" || name == "--jobs") {
            std::string v;
            status = take(&v);
            if (status.ok()) {
                const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), options.jobs);
                if (ec != std::errc() || end != v.data() + v.size() || options.jobs < 1 || options.jobs > 256) {
                    return Invalid(absl::StrCat(name, " takes a number of modules from 1 to 256, not \"", v, "\""));
                }
            }
        } else if (name == "--spin-list" && !value) {
            options.spin_list = true;
        } else {
            return Invalid(absl::StrCat("unknown option ", arg));
        }
        if (!status.ok()) return status;
    }
    const std::string& c = options.command;
    if (c == "help") return options;
    if (options.modules.empty()) return Invalid(absl::StrCat(c, " needs at least one module"));
    const bool rewrite = c == "rewrite";
    if (rewrite && options.out.empty()) return Invalid("rewrite needs -o <dir>");
    if (!rewrite && !options.out.empty()) return Invalid("info writes nothing: it takes no -o");
    if (!rewrite && (!options.game.empty() || !options.name.empty() || options.spin_list || options.jobs != 0)) {
        return Invalid("info takes only -p among the options");
    }
    if (!options.name.empty() && options.modules.size() != 1) return Invalid("--name names one module: give one file");
    if (!options.name.empty() && absl::StrContains(options.name, '/')) return Invalid("--name is a name, not a path");
    return options;
}

}  // namespace nxmac::nso
