// shader: a game's NVN shaders, split out for nxnative's shader build. shader help lists its commands.
#include <iostream>
#include <string>
#include <vector>

#include "shader/shader.h"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return nxmac::shader_tool::RunShader(args, std::cout, std::cerr);
}
