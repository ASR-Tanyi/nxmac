// nsp: the Switch packages of your games, read and checked. nsp help lists its commands.
#include <unistd.h>

#include <iostream>
#include <string>
#include <vector>

#include "nsp/nsp.h"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return nxmac::nsp::RunNsp(args, std::cout, std::cerr, ::isatty(STDERR_FILENO) != 0);
}
