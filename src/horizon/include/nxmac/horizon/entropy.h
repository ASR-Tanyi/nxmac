// The guest's entropy: GetInfo(RandomEntropy), and later getentropy and the csrng service's bytes, all from one source.
// The system's random numbers (arc4random), or with DM_DETCLOCK a fixed-seed splitmix64 stream, so that two runs of a
// replay draw the same (their pictures differed in the camp's blue fire particles before), with a log line at the
// first draw and every 1000th. A port of nxnative's dm/host/hle_os.c dm_entropy64.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

namespace nxmac::horizon {

class Entropy {
public:
    struct Options {
        bool deterministic = false;                 // DM_DETCLOCK is set
        std::function<void(std::string_view)> log;  // a log line
    };

    Entropy() : Entropy(Options{}) {}
    explicit Entropy(Options options) : options_(std::move(options)) {}

    std::uint64_t Next();  // dm_entropy64

private:
    Options options_;
    std::atomic<std::uint64_t> state_{0x5357444554524e47ull};  // "SWDETRNG"
    std::atomic<std::uint64_t> draws_{0};
};

}  // namespace nxmac::horizon
