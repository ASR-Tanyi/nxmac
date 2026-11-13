// shader's command line: shader <command> [options] [<RomFS>...]
#pragma once

#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace nxmac::shader_tool {

// The exit codes, as the usage text lists them
enum ExitCode : int {
    kOk = 0,
    // An error: a [RED] stop of the split, an archive whose reader is not ported, a file that cannot be read or written
    kFailed = 1,
    // The command line is wrong
    kUsage = 2,
};

struct Options {
    std::string command;  // split, build, profile or help
    std::vector<std::string>
        romfs;  // split: each version's RomFS, base game first; build: the package folder; profile: title IDs
    std::vector<std::string> mains;  // --main, one per RomFS, in the same order
    std::vector<std::string> dlc;    // --dlc
    std::string out;                 // -o
    std::string game;                // -g: the profile (base title ID)
    std::string profiles;            // -p: a profile table instead of the built-in one
    std::string glslc_capture;       // --glslc-capture
    int jobs = 0;                    // 0: one file per CPU
    // build
    std::string nxnative;            // --nxnative: nxnative's checkout, for the default profile folder and translator
    std::string profile;             // --profile: the game's profile folder
    std::string xlate;               // --xlate: the translator
    std::string tables;              // --tables
    std::string cache;               // --cache
    std::string min_macos = "15.0";  // --min-macos (none: "")
};

// args: the arguments after the program's name. InvalidArgument names what is wrong.
absl::StatusOr<Options> ParseOptions(const std::vector<std::string>& args);

std::string Usage();

}  // namespace nxmac::shader_tool
