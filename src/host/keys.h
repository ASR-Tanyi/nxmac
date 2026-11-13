// The host's four thread keys (nxnative's dm.h g_key18 / g_keytls / g_keytlr / g_keygate), in key_idx's order: K18 =
// the Switch's x18, KTLS = tpidr_el0, KTLR = tpidrro_el0 (the ThreadLocalRegion), KGATE = the thread's gate stub. Every
// module's key page holds their slot offsets, through which the rewritten code reads and writes them.
#pragma once

#include <pthread.h>

#include <array>

#include "nxmac/loader/image.h"
#include "nxmac/loader/layout.h"

namespace nxmac::host {

// Creates the keys and logs their numbers the first time (loader.c create_keys); the same keys after that
loader::HostKeys CreateKeys();
// The keys CreateKeys made
const std::array<pthread_key_t, loader::kKeyCount>& Keys();

}  // namespace nxmac::host
