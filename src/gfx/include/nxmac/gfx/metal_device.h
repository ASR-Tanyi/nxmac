// The Metal device the host renders with. A plain C++ interface: the Objective-C++ stays in metal_device.mm.
#pragma once

#include <string>

namespace nxmac::gfx {

// The GPU architecture of the system's default Metal device (e.g. applegpu_g14s); empty when there is no Metal device.
std::string DefaultGpuArchitecture();

}  // namespace nxmac::gfx
