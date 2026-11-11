#include "nsp/options.h"

#include <charconv>
#include <string_view>
#include <utility>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace nxmac::nsp {
namespace {

absl::Status Invalid(std::string_view message) { return absl::InvalidArgumentError(message); }

}  // namespace

std::string Usage() {
    return R"(nsp: the Switch packages of your games, read and checked (nxmac's rewrite of nxnative's nx_extract)

usage: nsp <command> [options] <package>...

commands:
  info <package>...             what the packages hold (content metas, NCAs, key generations), and whether
                                the keys open every NCA
  verify <package>...           read every file of every NCA through its hash checks, writing nothing; an
                                update's patch is checked over its base game when the base game's NSP is given too
  extract <game> [<update>] -o <dir>
                                the game, with its update laid over it: <dir>/ExeFS/, <dir>/RomFS.img (nxnative's
                                DMROMFS1 image), <dir>/control.nacp and <dir>/icon.jpg. The packages are told
                                apart by their content metas, in either order
  dlc <package>... -o <dir>     each add-on content's RomFS: <dir>/<index>.img, the index being the last three hex
                                digits of its title ID (nxnative's aoc/<index>.img)
  help                          this text

options:
  -k, --keys <file>     prod.keys (default ~/.switch/prod.keys); key values are never printed
  -o, --out <dir>       where extract and dlc write
  -j, --jobs <n>        threads that read and check (default: one per CPU)
      --romfs-dir       extract, dlc: the RomFS as a folder tree (<dir>/RomFS/, <dir>/<index>/) instead of an image
      --program <id>    extract: the program of a multi-program application (default: the lowest title ID)

exit codes: 0 done; 1 an error; 2 a wrong command line; 3 data that fails the hash checks (an incomplete copy or a
broken download: copy or download it again); 4 keys or title keys missing (prod.keys older than the game)
)";
}

absl::StatusOr<Options> ParseOptions(const std::vector<std::string>& args) {
    Options options;
    if (args.empty()) return Invalid("no command");
    options.command = args[0];
    if (options.command == "-h" || options.command == "--help") options.command = "help";
    const bool known = options.command == "info" || options.command == "verify" || options.command == "extract" ||
                       options.command == "dlc" || options.command == "help";
    if (!known) return Invalid(absl::StrCat("unknown command \"", options.command, "\""));

    bool only_packages = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (only_packages || arg.empty() || arg[0] != '-' || arg == "-") {
            options.packages.push_back(arg);
            continue;
        }
        if (arg == "--") {
            only_packages = true;
            continue;
        }
        // An option's value: "--name=value", or the next argument
        std::string name = arg;
        std::optional<std::string> value;
        if (const std::size_t eq = arg.find('='); absl::StartsWith(arg, "--") && eq != std::string::npos) {
            name = arg.substr(0, eq);
            value = arg.substr(eq + 1);
        }
        auto take_value = [&]() -> absl::StatusOr<std::string> {
            if (value) return *value;
            if (i + 1 >= args.size()) return Invalid(absl::StrCat(name, " needs a value"));
            return args[++i];
        };
        if (name == "-h" || name == "--help") {
            options.command = "help";
        } else if (name == "-k" || name == "--keys") {
            absl::StatusOr<std::string> v = take_value();
            if (!v.ok()) return v.status();
            options.keys = *v;
        } else if (name == "-o" || name == "--out") {
            absl::StatusOr<std::string> v = take_value();
            if (!v.ok()) return v.status();
            options.out = *v;
        } else if (name == "-j" || name == "--jobs") {
            absl::StatusOr<std::string> v = take_value();
            if (!v.ok()) return v.status();
            const auto [end, ec] = std::from_chars(v->data(), v->data() + v->size(), options.jobs);
            if (ec != std::errc() || end != v->data() + v->size() || options.jobs < 1 || options.jobs > 256) {
                return Invalid(absl::StrCat(name, " takes a number of threads from 1 to 256, not \"", *v, "\""));
            }
        } else if (name == "--program") {
            absl::StatusOr<std::string> v = take_value();
            if (!v.ok()) return v.status();
            absl::StatusOr<core::TitleId> id = core::TitleId::Parse(*v);
            if (!id.ok()) return Invalid(absl::StrCat("--program: ", id.status().message()));
            options.program = *id;
        } else if (name == "--romfs-dir" && !value) {
            options.romfs_dir = true;
        } else {
            return Invalid(absl::StrCat("unknown option ", arg));
        }
    }

    const std::string& c = options.command;
    if (c == "help") return options;
    if (options.packages.empty()) return Invalid(absl::StrCat(c, " needs at least one package"));
    if (c == "extract" && options.packages.size() > 2) {
        return Invalid("extract takes the game and, optionally, its update: two packages at most");
    }
    const bool writes = c == "extract" || c == "dlc";
    if (writes && options.out.empty()) return Invalid(absl::StrCat(c, " needs -o <dir>"));
    if (!writes && !options.out.empty()) return Invalid(absl::StrCat(c, " writes nothing: it takes no -o"));
    if (!writes && options.romfs_dir) return Invalid(absl::StrCat(c, " writes nothing: it takes no --romfs-dir"));
    if (c != "extract" && options.program) return Invalid("--program is extract's");
    return options;
}

}  // namespace nxmac::nsp
