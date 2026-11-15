// A guest thread's environment, as nxnative's dm/host/thread.c: its K18 slot (the Switch's x18) cleared, its KTLS slot
// (tpidr_el0) pointing at a zeroed TLS block, its KTLR slot (tpidrro_el0) at its ThreadLocalRegion (0x200 bytes), with
// the kernel's current thread object (0x400 bytes, zeroed) at TLR[0x1f8], and from SDK 22 the thread's ID at
// TLR[0x110]. The blocks are freed when the thread exits.
#pragma once

#include <cstdint>

namespace nxmac::host {

// guest_thread_init: before the thread runs guest code (again on the same thread: a fresh set, the old one freed)
void GuestThreadInit();
// guest_thread_set_id: the thread's kernel ID, the one svcGetThreadId answers (never 0)
void GuestThreadSetId(std::uint64_t id);

}  // namespace nxmac::host
