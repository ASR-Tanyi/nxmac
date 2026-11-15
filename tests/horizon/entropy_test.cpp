#include "nxmac/horizon/entropy.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace nxmac::horizon {
namespace {

using ::testing::ElementsAre;

TEST(EntropyTest, TheDeterministicStream) {
    std::vector<std::string> log;
    Entropy e({.deterministic = true, .log = [&](std::string_view l) { log.emplace_back(l); }});
    // splitmix64 from the seed "SWDETRNG": the values nxnative's host draws, the same in every run
    EXPECT_EQ(e.Next(), 0xff7ba408a5208807u);
    EXPECT_EQ(e.Next(), 0x51e67533e657687eu);
    EXPECT_EQ(e.Next(), 0xf700622382748e10u);
    for (int i = 3; i < 2000; ++i) e.Next();
    EXPECT_THAT(log,
                ElementsAre("DM_DETCLOCK: deterministic entropy call 1", "DM_DETCLOCK: deterministic entropy call 1000",
                            "DM_DETCLOCK: deterministic entropy call 2000"));
}

TEST(EntropyTest, TheSystemsOtherwise) {
    Entropy e;
    std::set<std::uint64_t> seen;
    for (int i = 0; i < 64; ++i) seen.insert(e.Next());
    EXPECT_GT(seen.size(), 60u);  // 64-bit random values do not repeat
    EXPECT_FALSE(seen.contains(0xff7ba408a5208807u));
}

}  // namespace
}  // namespace nxmac::horizon
