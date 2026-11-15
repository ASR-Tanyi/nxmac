#include "nxmac/horizon/address_space.h"

#include <cstdint>
#include <utility>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"

namespace nxmac::horizon {
namespace {

constexpr std::uint64_t kMacPage = 0x4000;

// BotW's profile (mem_reserve 4 GB, mem_alias and mem_stack 256 MB): the Mac's 64 GB alias region in place of the
// profile's, after a 3.25 GB heap region and the stacks
TEST(AddressSpaceTest, BotwsProfile) {
    const auto l =
        PlanAddressSpace({.reserve = 0x100000000, .alias = 0x10000000, .stack = 0x10000000}, kMacPage, nullptr);
    ASSERT_TRUE(l.ok()) << l.status();
    EXPECT_EQ(l->size, 0x10f0000000u);
    EXPECT_EQ(l->heap, 0x10000000u);
    EXPECT_EQ(l->heap_size, 0xd0000000u);
    EXPECT_EQ(l->stack, 0xe0000000u);
    EXPECT_EQ(l->stack_size, 0x10000000u);
    EXPECT_EQ(l->alias, 0xf0000000u);
    EXPECT_EQ(l->alias_size, kSwitchAliasSize);
}

TEST(AddressSpaceTest, AProfilesAliasRegionLargerThanTheSwitchsStays) {
    const auto l =
        PlanAddressSpace({.reserve = 0x1100000000, .alias = 0x1080000000, .stack = 0x10000000}, kMacPage, "");
    ASSERT_TRUE(l.ok()) << l.status();
    EXPECT_EQ(l->size, 0x1100000000u);
    EXPECT_EQ(l->heap_size, 0x60000000u);
    EXPECT_EQ(l->alias, 0x80000000u);
    EXPECT_EQ(l->alias_size, 0x1080000000u);
}

TEST(AddressSpaceTest, DmAliasSizeSetsTheAliasRegion) {
    const AddressSpaceProfile p{.reserve = 0x100000000, .alias = 0x10000000, .stack = 0x10000000};
    const auto l = PlanAddressSpace(p, kMacPage, "0x20000000");
    ASSERT_TRUE(l.ok()) << l.status();
    EXPECT_EQ(l->size, 0x110000000u);
    EXPECT_EQ(l->heap_size, 0xd0000000u);  // the heap and stacks as they are
    EXPECT_EQ(l->alias_size, 0x20000000u);
    // The profile's own value: an iPhone's address space
    const auto own = PlanAddressSpace(p, kMacPage, "268435456");
    ASSERT_TRUE(own.ok()) << own.status();
    EXPECT_EQ(own->size, p.reserve);
    // Under 256 MB, or not whole host pages
    for (const char* bad : {"0x1000", "0x10001000", "lots"}) {
        const auto r = PlanAddressSpace(p, kMacPage, bad);
        EXPECT_EQ(r.status().code(), absl::StatusCode::kInvalidArgument) << bad;
        EXPECT_EQ(r.status().message(), absl::StrCat("DM_ALIAS_SIZE ", bad, ": at least 256 MB, in whole host pages"));
    }
}

TEST(AddressSpaceTest, AProfileThatDoesNotFitStops) {
    const AddressSpaceProfile bad[] = {
        {.reserve = 0x8000000, .alias = 0, .stack = 0},                     // under the front
        {.reserve = 0x40000000, .alias = 0x38000000, .stack = 0},           // the alias region past the reserve
        {.reserve = 0x40000000, .alias = 0x10000000, .stack = 0x28000000},  // the stacks past it
        {.reserve = 0x40000000, .alias = 0x10000000, .stack = 0x1000},      // not whole host pages
    };
    for (const AddressSpaceProfile& p : bad) {
        const auto r = PlanAddressSpace(p, kMacPage, nullptr);
        EXPECT_EQ(r.status().code(), absl::StatusCode::kInvalidArgument) << p.reserve;
        EXPECT_EQ(r.status().message(), "guest address-space profile has invalid native boundaries");
    }
    // A heap region of nothing is a fit
    EXPECT_TRUE(
        PlanAddressSpace({.reserve = 0x30000000, .alias = 0x10000000, .stack = 0x10000000}, kMacPage, nullptr).ok());
}

TEST(AddressSpaceTest, TheBlockIsReservedWithoutAccess) {
    const auto l =
        PlanAddressSpace({.reserve = 0x40000000, .alias = 0x10000000, .stack = 0x8000000}, kMacPage, "0x10000000");
    ASSERT_TRUE(l.ok()) << l.status();
    auto as = GuestAddressSpace::Reserve(*l);
    ASSERT_TRUE(as.ok()) << as.status();
    ASSERT_NE(as->base(), nullptr);
    EXPECT_EQ(as->size(), 0x40000000u);
    EXPECT_EQ(as->heap(), as->base() + 0x10000000);
    EXPECT_EQ(as->stack(), as->base() + 0x28000000);
    EXPECT_EQ(as->alias(), as->base() + 0x30000000);
    const auto b = reinterpret_cast<std::uintptr_t>(as->base());
    EXPECT_TRUE(as->Contains(b));
    EXPECT_TRUE(as->Contains(b + 0x3fffffff));
    EXPECT_FALSE(as->Contains(b + 0x40000000));
    EXPECT_FALSE(as->Contains(b - 1));
    EXPECT_DEATH(*static_cast<volatile std::uint8_t*>(as->heap()) = 1, "");

    GuestAddressSpace moved = std::move(*as);
    EXPECT_EQ(moved.size(), 0x40000000u);
    EXPECT_EQ(as->base(), nullptr);
    EXPECT_EQ(as->heap(), nullptr);
    EXPECT_FALSE(as->Contains(b));
}

}  // namespace
}  // namespace nxmac::horizon
