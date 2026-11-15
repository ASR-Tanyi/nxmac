// The kernel's result codes the host's SVCs answer (nxnative's dm/host/hle_thread.c R_*): module 1, the description
// shifted by 9, as Atmosphère's svc::Result*. They come over with the SVCs that answer them.
#pragma once

#include <cstdint>

namespace nxmac::horizon {

inline constexpr std::uint32_t kResultInvalidHandle = 0xE401;  // 114

}  // namespace nxmac::horizon
