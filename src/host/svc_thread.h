// The thread and synchronization SVCs (nxnative's dm/host/hle_thread.c): each guest thread is one host thread, the
// SDK's nn::os runs on top as it is, and only Horizon's kernel under it is the host's. One lock (G) covers the thread
// table and the handles. So far: the main thread in the table and the thread queries (0x0C GetThreadPriority, 0x0E
// GetThreadCoreMask, 0x10 GetCurrentProcessorNumber, 0x25 GetThreadId). Not ported yet: the other threads
// (CreateThread...), the setters, the waits, the locks, the events, pausing (SetThreadActivity), DM_PREEMPT's
// same-core handoffs and the state dump.
#pragma once

#include <cstdint>

namespace nxmac::host {

// svc_thread_main_init: the calling thread registered as the main thread (priority 44, core 0, the first handle and
// ID: 0x1001, 0x51), named, its QoS class applied (DM_QOS_MAP, DM_QOS_EDGES) and logged. Returns its handle, which
// nnosInitialize gets.
std::uint32_t MainThreadInit();

// svc_thread: whether `num` is a thread SVC the host answers, its results written to `regs`
bool SvcThread(int num, std::uint64_t* regs);

}  // namespace nxmac::host
