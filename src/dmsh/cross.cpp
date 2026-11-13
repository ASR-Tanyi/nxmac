// SPIRV-Cross's own command line as a function: its main.cpp, with its main renamed, in this translation unit, so that
// main_inner (the whole of the program but for the try around it) can be called. The file is SPIRV-Cross's
// (Apache-2.0): nxmac's warnings and clang-tidy are off for this source.
#include "cross.h"

#include <cstdlib>
#include <exception>

#define main nxmac_spirv_cross_unused_main  // NOLINT
#include "main.cpp"                         // NOLINT: ${spirv_cross_SOURCE_DIR}/main.cpp
#undef main

namespace nxmac::dmsh {

CrossResult SpirvCross(const std::vector<std::string>& args) {
    std::vector<std::string> copy;
    copy.reserve(args.size() + 1);
    copy.emplace_back("spirv-cross");
    copy.insert(copy.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(copy.size() + 1);
    for (std::string& a : copy) argv.push_back(a.data());
    argv.push_back(nullptr);
    CrossResult result;
    try {
        result.ok = main_inner(static_cast<int>(copy.size()), argv.data()) == EXIT_SUCCESS;
        if (!result.ok) result.error = "spirv-cross failed";
    } catch (const std::exception& e) {
        result.error = std::string("SPIRV-Cross threw an exception: ") + e.what();
    }
    return result;
}

}  // namespace nxmac::dmsh
