// Programs the shader build runs (internal to nxmac::dmsh): nxnative's translator and Apple's Metal tools, as
// subprocess.run(..., capture_output=True) runs them.
#pragma once

#include <string>
#include <vector>

namespace nxmac::dmsh {

struct ProcessResult {
    int code = -1;  // the exit status; -1 when it could not start or was killed by a signal
    std::string out, err;
    std::string error;  // why it could not start
};

// argv[0] is run as it is when it holds a '/', else looked up on PATH. stdin_path: a file for its standard input
// ("": none). Safe to call from several threads.
ProcessResult RunProcess(const std::vector<std::string>& argv, const std::string& stdin_path = "");

// A command line as Python's subprocess shows it in CalledProcessError: Command '[...]' returned non-zero exit status
// n.
std::string CalledProcessError(const std::vector<std::string>& argv, int code);

}  // namespace nxmac::dmsh
