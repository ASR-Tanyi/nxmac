// The frame-step and stub functions of each game's builds (internal to nxmac::rewriter), ported from the profile's
// patches.py in nxnative; games/registry.cpp lists them by build ID.
#pragma once

#include "stubs.h"

namespace nxmac::rewriter::internal::games {

// The Legend of Zelda: Breath of the Wild, 1.9.0 (v1114112) main
void FrameStep01007EF00011E000V190(StubContext& ctx);

}  // namespace nxmac::rewriter::internal::games
