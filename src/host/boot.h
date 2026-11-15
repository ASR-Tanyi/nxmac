// The boot (nxnative's dm/host/main.c guest_main): the steps of the SDK's nn::init::Start in its own order, then
// nnMain. It runs on the calling thread, whose own stack the guest code then uses (its SVCs get a gate stack of their
// own).
#pragma once

#include <string>
#include <vector>

namespace nxmac::host {

// `args`: the game's arguments, "game" first (nn::os::SetHostArgc / SetHostArgv). Returns when nnMain does.
void GuestMain(const std::vector<std::string>& args);

}  // namespace nxmac::host
