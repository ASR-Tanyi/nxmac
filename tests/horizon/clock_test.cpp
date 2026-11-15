#include "nxmac/horizon/clock.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace nxmac::horizon {
namespace {

using ::testing::ElementsAre;

// The host's clock, as the test sets it
std::atomic<std::uint64_t> g_now{0};
std::uint64_t FakeNow() { return g_now.load(); }

GuestClock::Options Fake(std::uint64_t detclock_step = 0) {
    GuestClock::Options o;
    o.now = &FakeNow;
    o.detclock_step = detclock_step;
    return o;
}

TEST(GuestClockTest, FollowsTheHostAndCountsAtTheSwitchsRates) {
    g_now = 1000000000;  // 1 s
    const GuestClock clock(Fake());
    EXPECT_EQ(clock.Ns(), 1000000000u);
    EXPECT_EQ(clock.Ticks(false), 19200000u);  // 19.2 MHz
    EXPECT_EQ(clock.Ticks(true), 31250000u);   // 31.25 MHz (the Switch 2)
    g_now = 2500000000;
    EXPECT_EQ(clock.Ns(), 2500000000u);
    EXPECT_EQ(clock.NsAtHostTime(2000000000), 2000000000u);
}

TEST(GuestClockTest, AFreezeStandsStillAndGoesOnWithoutAJump) {
    g_now = 100;
    GuestClock clock(Fake());
    clock.Freeze(true);
    g_now = 200;
    EXPECT_EQ(clock.Ns(), 100u);
    EXPECT_EQ(clock.NsAtHostTime(150), 100u);
    clock.Freeze(false);
    EXPECT_EQ(clock.Ns(), 100u);
    g_now = 300;
    EXPECT_EQ(clock.Ns(), 200u);
    EXPECT_EQ(clock.NsAtHostTime(300), 200u);
}

TEST(GuestClockTest, FrozenWhileAnyReasonHolds) {
    g_now = 1000;
    GuestClock clock(Fake());
    clock.FreezeFor(kFreezeBackground, true);
    clock.FreezeFor(kFreezeKeyboard, true);
    g_now = 2000;
    clock.FreezeFor(kFreezeBackground, false);  // back in the foreground with the keyboard still up
    g_now = 3000;
    EXPECT_EQ(clock.Ns(), 1000u);
    clock.FreezeFor(kFreezeKeyboard, false);
    g_now = 3500;
    EXPECT_EQ(clock.Ns(), 1500u);
}

TEST(GuestClockTest, AReadWaitsWhileInTheBackground) {
    g_now = 0;
    GuestClock clock(Fake());
    clock.ReadWait(false);  // in the foreground: no wait
    EXPECT_EQ(GuestClock::ReadsOnThisThread(), 1u);
    clock.FreezeFor(kFreezeBackground, true);
    clock.ReadWait(true);  // host work reads the frozen clock
    EXPECT_EQ(GuestClock::ReadsOnThisThread(), 2u);
    std::atomic<bool> read{false};
    std::thread guest([&] {
        clock.ReadWait(false);
        read = true;
        EXPECT_EQ(GuestClock::ReadsOnThisThread(), 1u);  // its own count
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(read);
    clock.FreezeFor(kFreezeKeyboard, true);
    clock.FreezeFor(kFreezeBackground, false);  // the keyboard's freeze does not hold reads
    guest.join();
    EXPECT_TRUE(read);
}

TEST(GuestClockTest, TheDeterministicClockStepsByPresents) {
    g_now = 5000;
    std::vector<std::string> log;
    int advanced = 0;
    int frames = 0;
    GuestClock::Options o = Fake(/*detclock_step=*/16666667);
    o.log = [&](std::string_view l) { log.emplace_back(l); };
    o.on_advanced = [&] { ++advanced; };
    o.on_frame = [&] { ++frames; };
    GuestClock clock(std::move(o));
    EXPECT_FALSE(clock.DetclockEnabled());
    g_now = 6000;
    EXPECT_EQ(clock.Ns(), 6000u);  // the wall clock until the first present
    clock.FrameTick();
    EXPECT_TRUE(clock.DetclockEnabled());
    g_now = 9000000;
    EXPECT_EQ(clock.Ns(), 6000u);  // it goes on from there, by presents only
    clock.FrameTick();
    EXPECT_EQ(clock.Ns(), 6000u + 16666667);
    clock.Freeze(true);
    clock.FrameTick();  // no step while frozen
    EXPECT_EQ(clock.Ns(), 6000u + 16666667);
    EXPECT_EQ(clock.Frame(), 3u);
    EXPECT_EQ(frames, 3);
    EXPECT_EQ(advanced, 1);
    EXPECT_THAT(
        log,
        ElementsAre("DM_DETCLOCK: from the first present on, the guest clock advances by presents, step 16666667 ns"));
}

TEST(GuestClockTest, TheHostsClockMoves) {
    const std::uint64_t a = HostNs();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_GE(HostNs() - a, 1000000u);
}

}  // namespace
}  // namespace nxmac::horizon
