// nsp's command line: nsp <command> [options] <package>...
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "nxmac/core/title_id.h"

namespace nxmac::nsp {

// The exit codes, as the usage text lists them
enum ExitCode : int {
    kOk = 0,
    // An error: a package that does not open, a write that fails, a section nsp cannot read yet
    kFailed = 1,
    // The command line is wrong
    kUsage = 2,
    // Data fails the NCAs' hash checks: an incomplete copy or a broken download
    kDamaged = 3,
    // prod.keys lacks a key the packages need (it is older than the game), or a title key's ticket is missing
    kMissingKeys = 4,
};

struct Options {
    std::string command;  // info, verify, extract, dlc or help
    std::vector<std::string> packages;
    std::string keys;                      // prod.keys; empty: ~/.switch/prod.keys
    std::string out;                       // -o
    int jobs = 0;                          // 0: one thread per CPU
    std::optional<core::TitleId> program;  // extract: the program of a multi-program application
    bool romfs_dir = false;                // extract, dlc: a folder tree instead of a DMROMFS1 image
};

// args: the arguments after the program's name. InvalidArgument names what is wrong.
absl::StatusOr<Options> ParseOptions(const std::vector<std::string>& args);

std::string Usage();

}  // namespace nxmac::nsp
