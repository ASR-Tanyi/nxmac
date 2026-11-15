#include "nxmac/horizon/clock.h"

#include "absl/strings/str_format.h"

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

namespace nxmac::horizon {

std::uint64_t HostNs() {
#if defined(__APPLE__)
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return t;
    }();
    return mach_absolute_time() * tb.numer / tb.denom;
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000u + static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

std::uint64_t GuestClock::WallNs() const { return frozen_on_.load() ? frozen_.load() : options_.now() - lag_.load(); }

std::uint64_t GuestClock::Ns() const { return det_on_.load() ? det_.load() : WallNs(); }

std::uint64_t GuestClock::NsAtHostTime(std::uint64_t host_ns) const {
    if (det_on_.load()) return det_.load();
    if (frozen_on_.load()) return frozen_.load();
    const std::uint64_t lag = lag_.load();
    return host_ns >= lag ? host_ns - lag : 0;
}

std::uint64_t GuestClock::Ticks(bool ounce) const { return ounce ? Ns() / 32 : Ns() * 12 / 625; }

void GuestClock::FrameTick() {
    if (options_.on_frame) options_.on_frame();
    frame_.fetch_add(1);
    if (options_.detclock_step == 0) return;
    if (!det_on_.load()) {
        // From the first present on: it goes on from where the wall clock was, without a jump
        det_.store(WallNs());
        det_on_.store(true);
        Log(absl::StrFormat("DM_DETCLOCK: from the first present on, the guest clock advances by presents, step %d ns",
                            options_.detclock_step));
        return;
    }
    if (!frozen_on_.load()) {
        det_.fetch_add(options_.detclock_step);
        if (options_.on_advanced) options_.on_advanced();
    }
}

void GuestClock::FreezeFor(int reason, bool on) {
    const std::lock_guard<std::mutex> lock(mu_);
    if (on) {
        reasons_ |= 1u << reason;
    } else {
        reasons_ &= ~(1u << reason);
    }
    // guest_clock_freeze_apply
    if (reasons_ != 0 && !frozen_on_.load()) {
        frozen_.store(options_.now() - lag_.load());
        frozen_on_.store(true);
    } else if (reasons_ == 0 && frozen_on_.load()) {
        lag_.store(options_.now() - frozen_.load());
        frozen_on_.store(false);
    }
    const bool background = (reasons_ & (1u << kFreezeBackground)) != 0;
    background_.store(background);
    if (!background) cv_.notify_all();  // after the clock runs again: a released read sees it go on
}

void GuestClock::ReadWait(bool host_work) {
    ++ReadsOnThisThread();
    if (!background_.load() || host_work) return;
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return (reasons_ & (1u << kFreezeBackground)) == 0; });
}

std::uint64_t& GuestClock::ReadsOnThisThread() {
    thread_local std::uint64_t reads = 0;
    return reads;
}

}  // namespace nxmac::horizon
