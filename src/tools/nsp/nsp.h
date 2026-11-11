// nsp, run in-process: what its main runs, and what the tests drive.
#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace nxmac::nsp {

// Runs nsp with args (the arguments after the program's name), and returns its exit code (ExitCode). The report goes to
// out; errors, and with progress a line that updates itself while data is read (for a terminal), go to err.
int RunNsp(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, bool progress);

}  // namespace nxmac::nsp
