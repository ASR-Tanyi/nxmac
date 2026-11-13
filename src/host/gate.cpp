#include "gate.h"

#include <pthread.h>
#include <sys/mman.h>

#include <array>
#include <cerrno>
#include <mutex>

#include "keys.h"
#include "log.h"

// gate.S
extern "C" {
extern std::uint64_t nxhost_gate_top[nxmac::host::kGateSlots];
extern const std::uint8_t nxhost_gate_stubs[];
std::uint64_t nxhost_gate_dispatch(std::uint64_t ret, std::uint64_t* regs);
void nxhost_gate_nested(std::uint64_t sp);
}

namespace nxmac::host {
namespace {

constexpr std::size_t kGuardSize = 16384;  // below a mapped gate stack
constexpr std::size_t kStubSize = 16;

const loader::GateTable* g_table = nullptr;
GateHandlers g_handlers;

// The free slots; a thread's slot in its g_keygslot (a pointer to its number in g_slot_ids), given back by the key's
// destructor when the thread exits
std::mutex g_gate_lock;
std::array<std::uint16_t, kGateSlots> g_gate_free{};
std::array<std::uint16_t, kGateSlots> g_slot_ids{};
int g_gate_nfree = -1;
pthread_key_t g_keygslot;
pthread_once_t g_gslot_once = PTHREAD_ONCE_INIT;

void SlotRelease(void* p) {
    const std::lock_guard<std::mutex> lock(g_gate_lock);
    g_gate_free[static_cast<std::size_t>(g_gate_nfree++)] = *static_cast<const std::uint16_t*>(p);
}

void SlotKeyInit() {
    if (pthread_key_create(&g_keygslot, SlotRelease) != 0) Die("cannot create the gate slot key");
    if ((reinterpret_cast<std::uintptr_t>(nxhost_gate_top) & 0xfff) != 0) {
        Die("gate stack table %p is not page-aligned (gate.S's stubs address it with adrp alone)",
            static_cast<const void*>(nxhost_gate_top));
    }
}

}  // namespace

void SetGates(const loader::GateTable* table, const GateHandlers& handlers) {
    g_table = table;
    g_handlers = handlers;
}

__attribute__((noinline)) void GuestGateStack(bool own_stack) {
    pthread_once(&g_gslot_once, SlotKeyInit);
    std::uint8_t* top = nullptr;
    if (own_stack) {
        // This function's frame record: above it is the caller's frame, below it is free once the guest runs elsewhere
        top = static_cast<std::uint8_t*>(__builtin_frame_address(0));
    } else {
        void* const m =
            mmap(nullptr, kGateStackSize + kGuardSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED || mprotect(m, kGuardSize, PROT_NONE) != 0) {
            Die("cannot map a gate stack (errno %d)", errno);
        }
        top = static_cast<std::uint8_t*>(m) + kGuardSize + kGateStackSize;
    }
    const auto* const prev = static_cast<const std::uint16_t*>(pthread_getspecific(g_keygslot));
    int k = 0;
    if (prev != nullptr) {
        k = *prev;  // set again on the same thread: it keeps its slot
    } else {
        {
            const std::lock_guard<std::mutex> lock(g_gate_lock);
            if (g_gate_nfree < 0) {
                g_gate_nfree = 0;
                for (int i = 0; i < kGateSlots; ++i)
                    g_slot_ids[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(i);
                for (int i = kGateSlots - 1; i >= 0; --i)
                    g_gate_free[static_cast<std::size_t>(g_gate_nfree++)] = static_cast<std::uint16_t>(i);
            }
            if (g_gate_nfree == 0) Die("more than %d threads with a gate stack", kGateSlots);
            k = g_gate_free[static_cast<std::size_t>(--g_gate_nfree)];
        }
        pthread_setspecific(g_keygslot, &g_slot_ids[static_cast<std::size_t>(k)]);
    }
    nxhost_gate_top[k] = reinterpret_cast<std::uintptr_t>(top) & ~std::uint64_t{15};
    pthread_setspecific(Keys()[loader::kKeyGate], nxhost_gate_stubs + kStubSize * static_cast<std::size_t>(k));
}

}  // namespace nxmac::host

using nxmac::host::Die;

extern "C" void nxhost_gate_nested(std::uint64_t sp) {
    Die("SVC gate entered on its own gate stack (sp %#x): a gate handler ran guest code", sp);
}

extern "C" std::uint64_t nxhost_gate_dispatch(std::uint64_t ret, std::uint64_t* regs) {
    using nxmac::host::g_handlers;
    using nxmac::loader::GateKind;
    const nxmac::loader::Gate* const g = nxmac::host::g_table != nullptr ? nxmac::host::g_table->Find(ret) : nullptr;
    if (g == nullptr) Die("unknown gate at %#x", ret);
    switch (g->kind) {
        case GateKind::kCntpct:
        case GateKind::kCntvct: {
            // The system counter into the original instruction's register; the guest's x18 is its thread slot (the
            // gate leaves x18 alone: Apple's platform register)
            if (g_handlers.ticks == nullptr) Die("[RED] gate: the guest's system counter is not ported yet");
            const std::uint64_t t = g_handlers.ticks();
            if (g->arg == 18) {
                // The slot holds the guest's x18 itself, here the counter's value
                pthread_setspecific(nxmac::host::Keys()[nxmac::loader::kKeyK18],
                                    reinterpret_cast<void*>(t));  // NOLINT(performance-no-int-to-ptr)
            } else {
                regs[g->arg] = t;
            }
            return t;
        }
        case GateKind::kObs:
            return 0;  // no observation callbacks yet: the gate returns after one pass in and out
        case GateKind::kSpin:
            if (g_handlers.spin == nullptr) Die("[RED] gate: spin-wait passes are not ported yet");
            g_handlers.spin(ret, g->orig);
            return 0;
        case GateKind::kSvc:
            break;
    }
    if (g_handlers.svc == nullptr) Die("[RED] gate: svc %#x is not ported yet", g->arg);
    return g_handlers.svc(g->arg, regs, g->orig);
}
