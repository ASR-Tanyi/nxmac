#include "nxmac/loader/gates.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nxmac/rewriter/module.h"
#include "nxmac/rewriter/rewrite.h"

namespace nxmac::loader {
namespace {

TEST(GateTableTest, FindsWhatWasRegistered) {
    GateTable t;
    ASSERT_TRUE(t.Register(0x10000100, "svc", 31, 0x40).ok());
    ASSERT_TRUE(t.Register(0x10000200, "cntpct", 18, 0x80).ok());
    ASSERT_TRUE(t.Register(0x10000300, "cntvct", 3, 0xc0).ok());
    ASSERT_TRUE(t.Register(0x10000400, "obs", 5 | 7 << 8, 0x100).ok());
    ASSERT_TRUE(t.Register(0x10000500, "spin", 0, 0x140).ok());
    EXPECT_EQ(t.count(), 5);
    const Gate* g = t.Find(0x10000100);
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->kind, GateKind::kSvc);
    EXPECT_EQ(g->arg, 31);
    EXPECT_EQ(g->orig, 0x40u);
    EXPECT_EQ(t.Find(0x10000200)->kind, GateKind::kCntpct);
    EXPECT_EQ(t.Find(0x10000300)->kind, GateKind::kCntvct);
    EXPECT_EQ(t.Find(0x10000400)->arg >> 8, 7);
    EXPECT_EQ(t.Find(0x10000500)->kind, GateKind::kSpin);
    EXPECT_EQ(t.Find(0x10000104), nullptr);
    EXPECT_EQ(t.Find(0), nullptr);
    EXPECT_TRUE(t.obs_present(5));
    EXPECT_FALSE(t.obs_present(6));
}

TEST(GateTableTest, EightThousandGatesThatCollide) {
    GateTable t;
    // Return points 4 bytes apart in 32 MB steps, many to the same slot
    for (std::uint64_t i = 0; i < 8192; ++i) {
        ASSERT_TRUE(t.Register(0x100000000 + (i % 64) * 4 + (i / 64) * 0x2000000, "svc", static_cast<int>(i), i).ok());
    }
    for (std::uint64_t i = 0; i < 8192; ++i) {
        const Gate* g = t.Find(0x100000000 + (i % 64) * 4 + (i / 64) * 0x2000000);
        ASSERT_NE(g, nullptr) << i;
        EXPECT_EQ(g->orig, i);
    }
    EXPECT_EQ(t.Register(0x200000000, "svc", 1, 0).message(), "gate overflow");
}

TEST(GateTableTest, WhatStops) {
    GateTable t;
    EXPECT_EQ(t.Register(0x1000, "brk", 0, 0).message(), "unknown gate kind brk");
    EXPECT_EQ(t.Register(0x1000, "obs", 0, 0).message(), "observation gate number out of range 0");
    EXPECT_EQ(t.Register(0x1000, "obs", kObsMax, 0).message(), "observation gate number out of range 27");
    EXPECT_EQ(t.Register(0x1000, "obs", 0x100, 0).message(), "observation gate number out of range 0");
    ASSERT_TRUE(t.Register(0x1000, "svc", 1, 0).ok());
    EXPECT_EQ(t.Register(0x1000, "spin", 0, 0).message(), "gate registered twice 0x1000");
}

std::filesystem::path Folder(const std::string& test) {
    const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / "gates_test" / test;
    std::filesystem::create_directories(dir);
    return dir;
}

TEST(RegisterGatesTest, ReadsTheTableBesideTheImage) {
    const std::filesystem::path dir = Folder("read");
    std::ofstream(dir / "sdk.gates.tsv") << "0x1a4\tsvc\t31\t0x20\n"
                                            "0x1b0\tcntpct\t0\t0x30\n"
                                            "# not a gate\n"
                                            "0x1c0\tspin\n"
                                            "0x1d0 obs 261 0x40\n";
    constexpr std::uint64_t base = 0x100000000;
    GateTable t;
    const absl::StatusOr<int> n = RegisterGates(t, dir, "sdk", base);
    ASSERT_TRUE(n.ok()) << n.status();
    EXPECT_EQ(*n, 3);
    ASSERT_NE(t.Find(base + 0x1a4), nullptr);
    EXPECT_EQ(t.Find(base + 0x1a4)->arg, 31);
    EXPECT_EQ(t.Find(base + 0x1b0)->kind, GateKind::kCntpct);
    EXPECT_EQ(t.Find(base + 0x1d0)->arg, 261);
    EXPECT_TRUE(t.obs_present(5));
    // No table: no gates
    const absl::StatusOr<int> none = RegisterGates(t, dir, "main", base);
    ASSERT_TRUE(none.ok());
    EXPECT_EQ(*none, 0);
    // A bad row stops
    std::ofstream(dir / "bad.gates.tsv") << "0x10\tsmc\t0\t0x0\n";
    EXPECT_EQ(RegisterGates(t, dir, "bad", base).status().message(), "unknown gate kind smc");
}

// The gates the rewriter writes for its fixtures, every one registered and found
TEST(RegisterGatesTest, ReadsTheRewritersGates) {
    GateTable t;
    int total = 0;
    int i = 0;
    for (const char* file : {"sys.nso", "spin.nso", "data.nso"}) {
        const absl::StatusOr<rewriter::Module> module =
            rewriter::ReadModule((std::filesystem::path(NXMAC_REWRITER_FIXTURES) / file).string());
        ASSERT_TRUE(module.ok()) << module.status();
        rewriter::RewriteOptions options;
        options.name = "sdk";
        const absl::StatusOr<rewriter::RewriteResult> result = rewriter::Rewrite(*module, options);
        ASSERT_TRUE(result.ok()) << result.status();
        const std::filesystem::path dir = Folder(file);
        std::ofstream(dir / "sdk.gates.tsv") << result->gates;
        // Bases 4 GiB apart, as modules' are (only the address is used)
        const std::uint64_t base = std::uint64_t{0x100000000} * static_cast<std::uint64_t>(i + 1);
        const absl::StatusOr<int> n = RegisterGates(t, dir, "sdk", base);
        ASSERT_TRUE(n.ok()) << n.status();
        const std::vector<std::string_view> rows = absl::StrSplit(result->gates, '\n', absl::SkipEmpty());
        EXPECT_EQ(*n, static_cast<int>(rows.size())) << file;
        for (const std::string_view row : rows) {
            const std::vector<std::string_view> f = absl::StrSplit(row, '\t');
            EXPECT_NE(t.Find(base + std::stoull(std::string(f[0]), nullptr, 16)), nullptr) << file << ": " << row;
        }
        total += *n;
        ++i;
    }
    EXPECT_GT(total, 0);
    EXPECT_EQ(t.count(), total);
}

}  // namespace
}  // namespace nxmac::loader
