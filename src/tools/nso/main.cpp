// nso: Switch code modules, rewritten to run natively on Apple systems. nso help lists its commands.
#include <iostream>
#include <string>
#include <vector>

#include "nso/nso.h"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return nxmac::nso::RunNso(args, std::cout, std::cerr);
}
