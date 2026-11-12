// shader, run in-process: what its main runs, and what the tests drive.
#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace nxmac::shader_tool {

// Runs shader with args (the arguments after the program's name), and returns its exit code (ExitCode). The split's
// lines go to out, errors to err.
int RunShader(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace nxmac::shader_tool
