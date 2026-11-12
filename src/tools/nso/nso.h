// nso, run in-process: what its main runs, and what the tests drive.
#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace nxmac::nso {

// Runs nso with args (the arguments after the program's name), and returns its exit code (ExitCode). The report goes to
// out, errors to err.
int RunNso(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace nxmac::nso
