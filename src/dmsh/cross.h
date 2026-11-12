// SPIRV-Cross's command line, run in-process (internal to nxmac::dmsh). nxnative's shader build launches its
// spirv-cross binary for each shader; nxmac links SPIRV-Cross at the same commit and calls the command line's own
// code (its main.cpp is compiled into cross.cpp), so the same arguments give the same Metal source.
#pragma once

#include <string>
#include <vector>

namespace nxmac::dmsh {

struct CrossResult {
    bool ok = false;
    std::string error;  // what the program writes to its standard error when it fails
};

// args: the program's arguments (--msl ... <input.spv> --output <file>). The output goes where --output says. Safe to
// call from several threads.
CrossResult SpirvCross(const std::vector<std::string>& args);

}  // namespace nxmac::dmsh
