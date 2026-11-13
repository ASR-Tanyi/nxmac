// The host gate's C++ side: each thread's gate slot and gate stack (nxnative's thread.c guest_gate_stack), the check
// that no gate runs on its own gate stack (dm_gate_nested), and the dispatch of a gate by its kind (hle.c
// gate_dispatch). The assembly is gate.S. What an svc, a counter read or a spin-wait pass does is the host's: the
// handlers set with SetGates. Not ported yet: the thread state a gate enters and leaves (hle_thread.c thr_svc_enter /
// thr_svc_leave), the guest clock's wait while in the background, the observation callbacks.
#pragma once

#include <cstddef>
#include <cstdint>

#include "nxmac/loader/gates.h"

namespace nxmac::host {

inline constexpr int kGateSlots = 1024;  // threads with a gate stack at once (gate.S's .rept)
inline constexpr std::size_t kGateStackSize = std::size_t{1} << 20;  // gate.S's nesting check

struct GateHandlers {
    // svc #num: the return value, and any out-parameters written into regs (x0..x28 are read back)
    std::uint64_t (*svc)(int num, std::uint64_t* regs, std::uint64_t orig) = nullptr;
    std::uint64_t (*ticks)() = nullptr;  // mrs cntpct_el0 / cntvct_el0: the guest's system counter
    void (*spin)(std::uint64_t ret, std::uint64_t orig) = nullptr;  // a pass of a spin-wait loop, where it may sleep
};

// The gates and what they do, before any guest code runs
void SetGates(const loader::GateTable* table, const GateHandlers& handlers);

// Gives the calling thread a gate slot (kept if it has one) and a gate stack, and points its KGATE slot at the slot's
// stub. own_stack: the rest of this thread's own stack below the caller's frame (a thread whose guest code runs on
// another stack); else a newly mapped one with a guard page below it, never freed.
void GuestGateStack(bool own_stack);

}  // namespace nxmac::host
