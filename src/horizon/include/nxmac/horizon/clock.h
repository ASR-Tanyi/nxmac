// The guest clock: every system counter the guest reads (the cntpct / cntvct gates, svc GetSystemTick) comes from it.
// It follows the host's monotonic clock, but it can stand still: frozen for a reason (0: tests and loading screens, 1:
// the app in the background, 2: the system keyboard up), and it goes on after a freeze without a jump. A guest read
// waits while the app is in the background (a suspended app does not run on Horizon; a game that read the clock twice
// there saw no time pass and spun). With DM_DETCLOCK's step it advances by that much a present from the first present
// on, so two runs of a replay compare frame by frame. A port of nxnative's dm/host/hle_os.c (now_ns, guest_ns,
// guest_ns_at_host_time, guest_ticks, dm_guest_frame_tick, guest_clock_freeze_for, guest_clock_read_wait).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <utility>

namespace nxmac::horizon {

// The host's monotonic clock in nanoseconds (now_ns: mach_absolute_time on Apple's systems)
std::uint64_t HostNs();

enum FreezeReason : int { kFreezeHold = 0, kFreezeBackground = 1, kFreezeKeyboard = 2 };

class GuestClock {
public:
    struct Options {
        std::uint64_t (*now)() = &HostNs;           // the host's clock (a test's own)
        std::uint64_t detclock_step = 0;            // DM_DETCLOCK: nanoseconds a present; 0 = the wall clock
        std::function<void(std::string_view)> log;  // a log line
        std::function<void()> on_frame;             // each present, first (crash.c crash_watch_frame)
        std::function<void()> on_advanced;  // the deterministic clock moved (hle_thread.c dm_guest_clock_advanced)
    };

    GuestClock() : GuestClock(Options{}) {}
    explicit GuestClock(Options options) : options_(std::move(options)) {}

    std::uint64_t Ns() const;                                 // guest_ns
    std::uint64_t NsAtHostTime(std::uint64_t host_ns) const;  // guest_ns_at_host_time
    // The system counter: 19.2 MHz on the Switch, 31.25 MHz on the Switch 2 (`ounce`), whose SDK converts at that rate
    std::uint64_t Ticks(bool ounce) const;

    // A present (dm_guest_frame_tick): counts the frame, and with DM_DETCLOCK switches to (or steps) the clock
    void FrameTick();
    std::uint64_t Frame() const { return frame_.load(); }  // dm_guest_frame
    bool DetclockEnabled() const { return options_.detclock_step != 0 && det_on_.load(); }

    // guest_clock_freeze_for: frozen while any reason holds; reads waiting for the background go on when it ends
    void FreezeFor(int reason, bool on);
    void Freeze(bool on) { FreezeFor(kFreezeHold, on); }

    // A guest read of the counter (guest_clock_read_wait): counted on the calling thread, and it waits while the app
    // is in the background, but host work (inside a submit or present: `host_work`) reads the frozen clock instead
    void ReadWait(bool host_work);
    // This thread's guest reads of the counter (dm_clock_reads: thread yields tell waits for a time by them)
    static std::uint64_t& ReadsOnThisThread();

private:
    std::uint64_t WallNs() const;  // wall_guest_ns
    void Log(std::string_view line) const {
        if (options_.log) options_.log(line);
    }

    Options options_;
    std::atomic<std::uint64_t> frozen_{0};  // the guest time it froze at
    std::atomic<std::uint64_t> lag_{0};     // host time minus guest time, the sum of the freezes
    std::atomic<std::uint64_t> det_{0};     // the deterministic clock's time
    std::atomic<std::uint64_t> frame_{0};
    std::atomic<bool> frozen_on_{false};
    std::atomic<bool> det_on_{false};
    std::atomic<bool> background_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    unsigned reasons_ = 0;  // guarded by mu_
};

}  // namespace nxmac::horizon
