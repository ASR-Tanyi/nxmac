#include "thread.h"

#include <pthread.h>

#include <cstdlib>
#include <cstring>

#include "keys.h"
#include "log.h"

namespace nxmac::host {
namespace {

constexpr std::size_t kTlsSize = 8192;
constexpr std::size_t kTlrSize = 0x200;
constexpr std::size_t kThreadObjSize = 0x400;
constexpr std::size_t kTlrThreadObj = 0x1f8;  // the current thread object
constexpr std::size_t kTlrThreadId = 0x110;   // the thread's ID (SDK 22 compares it, SDK 16 the object)

// The blocks this thread allocated, freed by the key's destructor when it exits: the guest overwrites the KTLS / KTLR
// slots with its own thread pointer, so they cannot be freed through those (the old code leaked 9728 bytes a thread)
struct Own {
    void* tls;
    void* tlr;
};
pthread_key_t g_keyown;
pthread_once_t g_keyown_once = PTHREAD_ONCE_INIT;

void OwnFree(void* p) {
    auto* const own = static_cast<Own*>(p);
    std::free(own->tls);
    std::free(own->tlr);
    std::free(own);
}

void OwnKeyInit() {
    if (pthread_key_create(&g_keyown, &OwnFree) != 0) Die("cannot create the free key for thread-local blocks");
}

}  // namespace

void GuestThreadInit() {
    const auto& keys = Keys();
    pthread_setspecific(keys[loader::kKeyK18], nullptr);
    pthread_once(&g_keyown_once, &OwnKeyInit);
    auto* const own = static_cast<Own*>(std::calloc(1, sizeof(Own)));
    auto* const tlr = static_cast<std::uint8_t*>(std::calloc(1, kTlrSize + kThreadObjSize));
    if (own == nullptr || tlr == nullptr) Die("guest thread-local block allocation failed");
    own->tls = std::calloc(1, kTlsSize);
    own->tlr = tlr;
    if (own->tls == nullptr) Die("guest thread TLS block allocation failed");
    const auto thread_obj = reinterpret_cast<std::uintptr_t>(tlr + kTlrSize);
    std::memcpy(tlr + kTlrThreadObj, &thread_obj, sizeof thread_obj);
    pthread_setspecific(keys[loader::kKeyTls], own->tls);
    pthread_setspecific(keys[loader::kKeyTlr], tlr);
    if (void* const prev = pthread_getspecific(g_keyown)) OwnFree(prev);  // set again on the same thread
    pthread_setspecific(g_keyown, own);
}

void GuestThreadSetId(std::uint64_t id) {
    auto* const tlr = static_cast<std::uint8_t*>(pthread_getspecific(Keys()[loader::kKeyTlr]));
    const auto v = static_cast<std::uint32_t>(id);
    std::memcpy(tlr + kTlrThreadId, &v, sizeof v);
}

}  // namespace nxmac::host
