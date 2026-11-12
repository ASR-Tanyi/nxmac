// nso's command line: nso <command> [options] <module>...
#pragma once

#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace nxmac::nso {

// The exit codes, as the usage text lists them
enum ExitCode : int {
    kOk = 0,
    // An error: a module that does not load or cannot be rewritten, a file that cannot be written
    kFailed = 1,
    // The command line is wrong
    kUsage = 2,
};

struct Options {
    std::string command;  // rewrite, info or help
    std::vector<std::string> modules;
    std::string out;      // -o
    std::string game;     // -g: the profile (base title ID) whose function-matched patches apply
    std::string name;     // -n: the module's name, when its file is not named after it
    std::string patches;  // -p: patch tables instead of the built-in ones
    int jobs = 0;         // 0: one module per CPU
    bool spin_list = false;
};

// args: the arguments after the program's name. InvalidArgument names what is wrong.
absl::StatusOr<Options> ParseOptions(const std::vector<std::string>& args);

std::string Usage();

}  // namespace nxmac::nso
