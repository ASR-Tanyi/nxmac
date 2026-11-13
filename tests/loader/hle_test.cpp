#include "nxmac/loader/hle.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "elf_builder.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nxmac/loader/relocate.h"
#include "nxmac/loader/symbols.h"

namespace nxmac::loader {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

// Functions only for their addresses
void HostMemcpy() {}
void HostStrlen() {}
void HostOpen() {}
void HostClose() {}
void HostIoctl() {}

const HleEntry kHle[] = {{"memcpy", reinterpret_cast<const void*>(&HostMemcpy)},
                         {"strlen", reinterpret_cast<const void*>(&HostStrlen)},
                         {"memcpy", reinterpret_cast<const void*>(&HostClose)}};  // a second memcpy: the first wins
const HleForceEntry kForce[] = {
    {"NvOsDrvOpen", reinterpret_cast<const void*>(&HostOpen), "the host has no nvdrv"},
    {"NvOsDrvClose", reinterpret_cast<const void*>(&HostClose), "nothing to do"},
    {"NvOsDrvIoctl", reinterpret_cast<const void*>(&HostIoctl), "returns an error"},
};

std::uint64_t Addr(void (*f)()) { return reinterpret_cast<std::uint64_t>(f); }

constexpr std::uint64_t kStubs = 0x7000000000;  // where the made-up stub table is

struct Logged {
    std::vector<std::string> lines;
    HleOptions Options(bool log_imports = false) {
        HleOptions o;
        o.log_imports = log_imports;
        o.log = [this](std::string_view l) { lines.emplace_back(l); };
        o.describe = [](const void*) { return std::string("host_fn"); };
        o.stub = [](unsigned k) { return kStubs + 8 * std::uint64_t{k}; };
        return o;
    }
};

TEST(HleResolverTest, TakeoversImplementationsAndStubs) {
    Logged log;
    absl::StatusOr<HleResolver> r = HleResolver::Create(kHle, kForce, log.Options());
    ASSERT_TRUE(r.ok()) << r.status();
    EXPECT_EQ(r->Forced("NvOsDrvOpen"), Addr(&HostOpen));
    EXPECT_EQ(r->Forced("NvOsDrvOpen"), Addr(&HostOpen));
    EXPECT_EQ(r->Forced("memcpy"), 0u);  // an implementation, not a takeover
    EXPECT_TRUE(r->Implements("memcpy"));
    EXPECT_TRUE(r->Implements("strlen"));
    EXPECT_FALSE(r->Implements("NvOsDrvOpen"));
    EXPECT_FALSE(r->Implements("nnMain"));
    EXPECT_EQ(*r->Import("main", "memcpy"), Addr(&HostMemcpy));  // the first memcpy
    // Named stubs: numbered in order, one per name
    EXPECT_EQ(*r->Import("main", "nn_unknown_a"), 0x7000000000u);
    EXPECT_EQ(*r->Import("sdk", "nn_unknown_b"), 0x7000000008u);
    EXPECT_EQ(*r->Import("sdk", "nn_unknown_a"), 0x7000000000u);
    EXPECT_EQ(r->stubs(), 2u);
    EXPECT_EQ(r->stub_name(1), "nn_unknown_b");
    EXPECT_THAT(log.lines, IsEmpty());  // nothing logged without DM_IMPORTS
    ASSERT_TRUE(r->CheckForced(false).ok());
    EXPECT_THAT(log.lines, ElementsAre("force: 1 entries hit, 2 not referenced by this game"));
}

TEST(HleResolverTest, WithImportsLogged) {
    Logged log;
    absl::StatusOr<HleResolver> r = HleResolver::Create(kHle, kForce, log.Options(/*log_imports=*/true));
    ASSERT_TRUE(r.ok()) << r.status();
    r->Forced("NvOsDrvIoctl");
    r->Forced("NvOsDrvIoctl");  // once per entry
    ASSERT_TRUE(r->Import("main", "strlen").ok());
    ASSERT_TRUE(r->CheckForced(false).ok());
    EXPECT_THAT(log.lines, ElementsAre("force NvOsDrvIoctl  (returns an error)", "import strlen -> host_fn",
                                       "forced takeover entry with no reference at all: NvOsDrvOpen",
                                       "forced takeover entry with no reference at all: NvOsDrvClose",
                                       "force: 1 entries hit, 2 not referenced by this game"));
}

TEST(HleResolverTest, StrictCheckingStopsOnAnUnusedTakeover) {
    Logged log;
    absl::StatusOr<HleResolver> r = HleResolver::Create({}, kForce, log.Options());
    ASSERT_TRUE(r.ok());
    r->Forced("NvOsDrvOpen");
    r->Forced("NvOsDrvClose");
    EXPECT_EQ(r->CheckForced(true).message(), "forced takeover list is wrong");
    EXPECT_THAT(log.lines, ElementsAre("forced takeover entry with no reference at all: NvOsDrvIoctl"));
    r->Forced("NvOsDrvIoctl");
    log.lines.clear();
    EXPECT_TRUE(r->CheckForced(true).ok());
    EXPECT_THAT(log.lines, ElementsAre("force: 3 entries hit, 0 not referenced by this game"));
}

TEST(HleResolverTest, ATakeoverListedTwiceStops) {
    const HleForceEntry twice[] = {{"a", nullptr, "x"}, {"b", nullptr, "y"}, {"a", nullptr, "z"}};
    EXPECT_EQ(HleResolver::Create({}, twice, {}).status().message(), "duplicate entry in the forced takeover list: a");
}

TEST(HleResolverTest, FourThousandNinetySixStubs) {
    Logged log;
    absl::StatusOr<HleResolver> r = HleResolver::Create({}, {}, log.Options());
    ASSERT_TRUE(r.ok());
    for (unsigned k = 0; k < kMaxStubs; ++k) ASSERT_EQ(*r->Import("main", absl::StrCat("f", k)), kStubs + 8 * k);
    for (unsigned k = 0; k < kMaxStubs; k += 97) EXPECT_EQ(*r->Import("sdk", absl::StrCat("f", k)), kStubs + 8 * k);
    EXPECT_EQ(r->Import("main", "one_more").status().message(), "stub overflow");
}

// Through relocation: a takeover wins over the module's own definition, and a stub overflow stops it
TEST(HleResolverTest, InRelocation) {
    testing::ElfBuilder b;
    b.Rela(testing::kData, b.Sym("NvOsDrvOpen", 1, 0x40), kRAarch64GlobDat);
    b.Rela(testing::kData + 8, b.Sym("memcpy", 0, 0), kRAarch64GlobDat);
    b.Rela(testing::kData + 16, b.Sym("nn_unknown", 0, 0), kRAarch64GlobDat);
    std::vector<std::uint8_t> img = b.Build();
    absl::StatusOr<ModuleSymbols> s = ModuleSymbols::Parse(img.data(), img.size(), "main");
    ASSERT_TRUE(s.ok());
    const LoadedModule modules[] = {{"main", img.data(), img.size(), 0, 0, &*s}};
    Logged log;
    absl::StatusOr<HleResolver> r = HleResolver::Create(kHle, kForce, log.Options());
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(Relocate(modules, 0, *r).ok());
    const auto word = [&](std::uint64_t off) {
        std::uint64_t v;
        std::memcpy(&v, img.data() + off, 8);
        return v;
    };
    EXPECT_EQ(word(testing::kData), Addr(&HostOpen));
    EXPECT_EQ(word(testing::kData + 8), Addr(&HostMemcpy));
    EXPECT_EQ(word(testing::kData + 16), 0x7000000000u);
    ASSERT_TRUE(r->CheckForced(false).ok());
    EXPECT_THAT(log.lines, ElementsAre("force: 1 entries hit, 2 not referenced by this game"));

    absl::StatusOr<HleResolver> full = HleResolver::Create({}, {}, log.Options());
    ASSERT_TRUE(full.ok());
    for (unsigned k = 0; k < kMaxStubs; ++k) ASSERT_TRUE(full->Import("x", absl::StrCat("f", k)).ok());
    EXPECT_EQ(Relocate(modules, 0, *full).status().message(), "stub overflow");
}

}  // namespace
}  // namespace nxmac::loader
