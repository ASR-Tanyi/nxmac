#include "nxmac/horizon/entropy.h"

#include <cstdlib>

#include "absl/strings/str_format.h"

namespace nxmac::horizon {

std::uint64_t Entropy::Next() {
    if (!options_.deterministic) return (static_cast<std::uint64_t>(arc4random()) << 32) | arc4random();
    constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_.fetch_add(kGolden) + kGolden;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    const std::uint64_t n = draws_.fetch_add(1) + 1;
    if ((n == 1 || n % 1000 == 0) && options_.log) {
        options_.log(absl::StrFormat("DM_DETCLOCK: deterministic entropy call %d", n));
    }
    return z;
}

}  // namespace nxmac::horizon
