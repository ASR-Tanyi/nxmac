#include "svc_thread.h"

#include <pthread.h>
#include <pthread/qos.h>

#include <cstdint>
#include <cstdlib>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/thread_annotations.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "log.h"
#include "nxmac/horizon/handles.h"
#include "nxmac/horizon/results.h"
#include "thread.h"

namespace nxmac::host {
namespace {

using horizon::ObjectType;

// A guest thread (hle_thread.c thr_t): so far what the queries read
struct GuestThread {
    std::uint32_t handle = 0;
    std::uint64_t id = 0;
    int prio = 0;
    int core = 0;            // its ideal core; negative for none
    std::uint64_t mask = 0;  // the cores it may run on
    bool started = false;
    bool entered = false;  // its host thread has been dispatched and goes on to guest code
    pthread_t pt{};
    std::uint32_t qos = 0;  // its host thread's QoS class
};

// G: the thread table's one lock, over the handles, the threads' fields and (later) every wait. Constant-initialized:
// no code runs for it at startup, so nothing can throw there.
ABSL_CONST_INIT absl::Mutex g_lock(absl::kConstInit);  // NOLINT(bugprone-throwing-static-initialization)
horizon::HandleTable g_handles ABSL_GUARDED_BY(g_lock);
std::uint64_t g_next_id ABSL_GUARDED_BY(g_lock) = 0x51;

// The calling host thread's GuestThread (g_keythr)
pthread_key_t ThreadKey() {
    static const pthread_key_t key = [] {
        pthread_key_t k{};
        if (pthread_key_create(&k, nullptr) != 0) Die("thread table: no thread key");
        return k;
    }();
    return key;
}

GuestThread* Self() { return static_cast<GuestThread*>(pthread_getspecific(ThreadKey())); }

// Switch priority → QoS class (DM_QOS_MAP=1; off by default until a phone A/B shows it saves power without dropping
// frames). Otherwise every guest thread runs at user-interactive whatever its priority, and the performance controller
// keeps all of them on fast cores at high clocks. Bands: priority <= e1 (the main thread is 44) user-interactive,
// <= e2 user-initiated, above that utility; DM_QOS_EDGES=e1,e2 moves them (default 44,59). Read once in
// MainThreadInit, before any other guest thread exists.
struct QosMap {
    bool on = false;
    int e1 = 44;
    int e2 = 59;
};
QosMap g_qos;

void QosMapInit() {
    const char* const e = std::getenv("DM_QOS_MAP");
    const char* const d = std::getenv("DM_QOS_EDGES");
    g_qos.on = e != nullptr && std::strtol(e, nullptr, 10) != 0;
    if (d != nullptr) {  // as sscanf "%d,%d": an edge that does not parse stays as it is, and so does the rest
        char* end = nullptr;
        const long e1 = std::strtol(d, &end, 10);
        if (end != d) {
            g_qos.e1 = static_cast<int>(e1);
            if (*end == ',') {
                const char* const s = end + 1;
                const long e2 = std::strtol(s, &end, 10);
                if (end != s) g_qos.e2 = static_cast<int>(e2);
            }
        }
    }
    if (g_qos.on) {
        Log("DM_QOS_MAP=1: guest threads get QoS by priority (≤%d interactive, ≤%d user-initiated, the rest utility)",
            g_qos.e1, g_qos.e2);
    }
}

qos_class_t PrioQos(int prio) {
    if (!g_qos.on) return QOS_CLASS_USER_INTERACTIVE;
    return prio <= g_qos.e1 ? QOS_CLASS_USER_INTERACTIVE
                            : (prio <= g_qos.e2 ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY);
}

// On the thread itself (only a thread can set its own QoS)
void QosApply(GuestThread* t) ABSL_EXCLUSIVE_LOCKS_REQUIRED(g_lock) {
    const qos_class_t q = PrioQos(t->prio);
    if (static_cast<std::uint32_t>(q) != t->qos && pthread_set_qos_class_self_np(q, 0) == 0) {
        t->qos = static_cast<std::uint32_t>(q);
    }
}

// h_get: the thread `handle` names, the calling thread for the pseudo-handle; a wrong handle is logged (it is a real
// error)
GuestThread* GetThread(std::uint32_t handle) ABSL_EXCLUSIVE_LOCKS_REQUIRED(g_lock) {
    GuestThread* const t = handle == horizon::kCurrentThreadHandle
                               ? Self()
                               : static_cast<GuestThread*>(g_handles.Peek(handle, ObjectType::kThread));
    if (t == nullptr) Log("invalid handle %#x (wanted type %d)", handle, static_cast<int>(ObjectType::kThread));
    return t;
}

// cur: the calling thread, which a thread SVC always has
GuestThread* Current() {
    GuestThread* const t = Self();
    if (t == nullptr) Die("thread SVC called from a non-guest thread");
    return t;
}

}  // namespace

std::uint32_t MainThreadInit() {
    QosMapInit();
    auto* const t = new GuestThread;  // the main thread's, for the process's life
    // Its affinity is its ideal core alone, as Horizon creates every thread (Mesosphere KThread::Initialize)
    t->started = true;
    t->entered = true;
    t->prio = 44;
    t->core = 0;
    t->mask = 1;
    t->pt = pthread_self();
    {
        absl::MutexLock lock(g_lock);
        QosApply(t);
        qos_class_t qc = QOS_CLASS_UNSPECIFIED;
        int relative = 0;
        pthread_get_qos_class_np(pthread_self(), &qc, &relative);
        t->qos = static_cast<std::uint32_t>(qc);
        const absl::StatusOr<std::uint32_t> handle = g_handles.New(ObjectType::kThread, t);
        if (!handle.ok()) Die("%s", handle.status().message());
        t->handle = *handle;
        t->id = g_next_id++;
    }
    pthread_setspecific(ThreadKey(), t);
    GuestThreadSetId(t->id);  // GuestMain ran GuestThreadInit before this, and no guest code has run yet
    pthread_setname_np("0x1001 main thread");
    Log("main guest thread QoS %#x (priority %d)", t->qos, t->prio);
    return t->handle;
}

bool SvcThread(int num, std::uint64_t* regs) {
    switch (num) {
        case 0x0C: {  // GetThreadPriority(→w1 priority; w1 handle)
            absl::MutexLock lock(g_lock);
            const GuestThread* const t = GetThread(static_cast<std::uint32_t>(regs[1]));
            regs[0] = t != nullptr ? 0 : horizon::kResultInvalidHandle;
            regs[1] = t != nullptr ? static_cast<std::uint64_t>(t->prio) : 0;
            return true;
        }
        case 0x0E: {  // GetThreadCoreMask(→w1 ideal core, →x2 affinity mask; w2 handle)
            absl::MutexLock lock(g_lock);
            const GuestThread* const t = GetThread(static_cast<std::uint32_t>(regs[2]));
            regs[0] = t != nullptr ? 0 : horizon::kResultInvalidHandle;
            regs[1] = t != nullptr ? static_cast<std::uint32_t>(t->core) : 0;
            regs[2] = t != nullptr ? t->mask : 0;
            return true;
        }
        // GetCurrentProcessorNumber(→w0): always 0..2, never negative; a thread with no ideal core reports the lowest
        // core its mask allows, not core 0 outside it
        case 0x10: {
            const GuestThread* const t = Current();
            absl::MutexLock lock(g_lock);
            const int c = t->core;
            regs[0] = static_cast<std::uint32_t>(c >= 0 ? c : (t->mask != 0 ? __builtin_ctzll(t->mask) : 0));
            return true;
        }
        case 0x25: {  // GetThreadId(→x1 ID; w1 handle)
            absl::MutexLock lock(g_lock);
            const GuestThread* const t = GetThread(static_cast<std::uint32_t>(regs[1]));
            regs[0] = t != nullptr ? 0 : horizon::kResultInvalidHandle;
            regs[1] = t != nullptr ? t->id : 0;
            return true;
        }
        default:
            return false;
    }
}

}  // namespace nxmac::host
