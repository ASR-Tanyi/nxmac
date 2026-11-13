#include "keys.h"

#include <cstdint>

#include "log.h"

namespace nxmac::host {
namespace {

std::array<pthread_key_t, loader::kKeyCount> g_keys{};

}  // namespace

loader::HostKeys CreateKeys() {
    static bool created = false;  // before any guest thread: no lock needed
    if (!created) {
        for (pthread_key_t& k : g_keys) {
            if (pthread_key_create(&k, nullptr) != 0) Die("cannot create the host key");
        }
        created = true;
        Log("key numbers K18=%lu KTLS=%lu KTLR=%lu KGATE=%lu", static_cast<unsigned long>(g_keys[loader::kKeyK18]),
            static_cast<unsigned long>(g_keys[loader::kKeyTls]), static_cast<unsigned long>(g_keys[loader::kKeyTlr]),
            static_cast<unsigned long>(g_keys[loader::kKeyGate]));
    }
    loader::HostKeys keys{};
    for (int k = 0; k < loader::kKeyCount; ++k) keys[k] = static_cast<std::uint64_t>(g_keys[k]);
    return keys;
}

const std::array<pthread_key_t, loader::kKeyCount>& Keys() { return g_keys; }

}  // namespace nxmac::host
