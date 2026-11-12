// nxmac's rewriter held to nxnative's: the fixtures of tests/rewriter/fixtures (small modules that hold every kind of
// rewrite site, compiled code, a registered build and an update of it, a frame step) are rewritten and each file must
// have the SHA-256 that nxnative's patch_nso.py gave for it (expected.tsv, written by fixtures/src/make_fixtures.sh).
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nxmac/fs/crypto.h"
#include "nxmac/rewriter/module.h"
#include "nxmac/rewriter/patches.h"
#include "nxmac/rewriter/rewrite.h"

namespace nxmac::rewriter {
namespace {

using ::testing::HasSubstr;

// The folder of the fixtures (tests/rewriter/fixtures)
std::string Fixture(std::string_view file) { return std::string(NXMAC_REWRITER_FIXTURES "/") + std::string(file); }

struct Case {
    std::string name, file, module, game, want;
};

std::vector<std::vector<std::string>> Rows(const std::string& file) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream in(Fixture(file));
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        rows.push_back(absl::StrSplit(line, '\t'));
    }
    return rows;
}

std::vector<Case> Cases() {
    std::vector<Case> cases;
    for (const std::vector<std::string>& row : Rows("cases.tsv")) {
        if (row.size() == 5) cases.push_back({row[0], row[1], row[2], row[3] == "-" ? "" : row[3], row[4]});
    }
    return cases;
}

const PatchTables& FixtureTables() {
    static const PatchTables* const tables = [] {
        absl::StatusOr<PatchTables> read = PatchTables::ReadFile(Fixture("patches_fixture.tsv"));
        EXPECT_TRUE(read.ok()) << read.status();
        return new PatchTables(read.ok() ? *std::move(read) : PatchTables());
    }();
    return *tables;
}

std::string Sha256Hex(const void* data, std::size_t size) {
    const fs::Sha256Hash hash = fs::Sha256(std::span(static_cast<const std::uint8_t*>(data), size));
    return absl::BytesToHexString(std::string_view(reinterpret_cast<const char*>(hash.data()), hash.size()));  // NOLINT
}

class GoldenTest : public ::testing::TestWithParam<Case> {};

TEST_P(GoldenTest, RewritesAsNxnativeDoes) {
    const Case& c = GetParam();
    const absl::StatusOr<Module> module = ReadModule(Fixture(c.file));
    ASSERT_TRUE(module.ok()) << module.status();
    RewriteOptions options;
    options.name = c.module;
    options.game = c.game;
    options.tables = &FixtureTables();
    const absl::StatusOr<RewriteResult> result = Rewrite(*module, options);
    if (c.want != "ok") {
        ASSERT_FALSE(result.ok());
        EXPECT_THAT(std::string(result.status().message()), HasSubstr(c.want));
        return;
    }
    ASSERT_TRUE(result.ok()) << result.status();
    std::map<std::string, std::string> expected;
    for (const std::vector<std::string>& row : Rows("expected.tsv")) {
        if (row.size() == 3 && row[0] == c.name) expected[row[1]] = row[2];
    }
    ASSERT_EQ(expected.size(), 8u) << "expected.tsv has no hashes for the case " << c.name;
    const std::pair<const char*, std::string> files[] = {
        {"patched.bin", Sha256Hex(result->patched.data(), result->patched.size())},
        {"fixups.tsv", Sha256Hex(result->fixups.data(), result->fixups.size())},
        {"gates.tsv", Sha256Hex(result->gates.data(), result->gates.size())},
        {"sites.tsv", Sha256Hex(result->sites.data(), result->sites.size())},
        {"cuts.tsv", Sha256Hex(result->cuts.data(), result->cuts.size())},
        {"changed.tsv", Sha256Hex(result->changed.data(), result->changed.size())},
        {"obs.tsv", Sha256Hex(result->obs.data(), result->obs.size())},
        {"match.tsv", result->match ? Sha256Hex(result->match->data(), result->match->size()) : "-"},
    };
    for (const auto& [file, hash] : files) EXPECT_EQ(hash, expected[file]) << c.module << "." << file;
}

INSTANTIATE_TEST_SUITE_P(Fixtures, GoldenTest, ::testing::ValuesIn(Cases()),
                         [](const ::testing::TestParamInfo<Case>& info) { return info.param.name; });

TEST(GoldenFixturesTest, AreThere) {
    EXPECT_GE(Cases().size(), 15u) << "no cases in " << Fixture("cases.tsv");
    EXPECT_GT(FixtureTables().build_count(), 0u);
}

// What the fixtures' files say, beyond their hashes
RewriteResult RewriteFixture(const std::string& file, const std::string& name, const std::string& game = "") {
    const absl::StatusOr<Module> module = ReadModule(Fixture(file));
    EXPECT_TRUE(module.ok()) << module.status();
    RewriteOptions options;
    options.name = name;
    options.game = game;
    options.tables = &FixtureTables();
    absl::StatusOr<RewriteResult> result = Rewrite(*module, options);
    EXPECT_TRUE(result.ok()) << result.status();
    return result.ok() ? *std::move(result) : RewriteResult();
}

TEST(RewriteTest, TheFixupsNameTheLayout) {
    const RewriteResult plain = RewriteFixture("sys.nso", "sdk");
    EXPECT_THAT(plain.fixups, HasSubstr("# PATCH_OFF\t0x10000\n"));
    EXPECT_THAT(plain.fixups,
                HasSubstr("# KEY_K18_IDX\t0\n# KEY_KTLS_IDX\t1\n# KEY_KTLR_IDX\t2\n# KEY_KGATE_IDX\t3\n"));
    EXPECT_THAT(plain.gates, HasSubstr("\tsvc\t31\t"));
    EXPECT_THAT(plain.summary, HasSubstr("'svc': 3"));
    // 128 MB of bss: the patch area goes before the base, and the file holds no bss
    const RewriteResult far = RewriteFixture("far.nso", "main");
    EXPECT_THAT(far.fixups, HasSubstr("# PATCH_OFF\t-0x2000000\n"));
    EXPECT_THAT(far.fixups, HasSubstr("# IMAGE_SIZE\t"));
    EXPECT_LT(far.patched.size(), std::size_t{0x200000});
}

TEST(RewriteTest, AFrameStepGetsItsStatePage) {
    const RewriteResult result = RewriteFixture("botw_sites.nso", "main", "01007EF00011E000");
    EXPECT_THAT(result.fixups, HasSubstr("# FPS_STATE_OFF\t0x154c000\n# RENDER_SCALE\t1\n"));
    EXPECT_THAT(result.cuts, HasSubstr("0x11269b8\tfd7bbea9\t"));
    EXPECT_THAT(result.summary, HasSubstr("'frame_step': 43} cuts 50"));
    // The stubs are main's: the same build under another name has only its cuts
    const RewriteResult other = RewriteFixture("botw_sites.nso", "sdk");
    EXPECT_THAT(other.fixups, ::testing::Not(HasSubstr("FPS_STATE_OFF")));
    EXPECT_THAT(other.summary, HasSubstr("cuts 7 "));
}

TEST(RewriteTest, AnUnregisteredBuildSaysWhatItKept) {
    const RewriteResult result = RewriteFixture("g2.nso", "main", "0100000000009000");
    ASSERT_TRUE(result.match.has_value());
    EXPECT_THAT(*result.match, HasSubstr("not a registered build"));
    EXPECT_THAT(*result.match, HasSubstr("\nregistered\t480046dbad79a8218759d4f247340434\t1.0.0\t3\n"));
    EXPECT_THAT(*result.match, HasSubstr("\npatch\tone function\t0x"));
    EXPECT_THAT(*result.match, HasSubstr("\npatch\tno such function\t-\t0 matching functions (need exactly one)\n"));
    EXPECT_THAT(result.cuts, HasSubstr("one function [function match]\n"));
    bool red = false;
    for (const std::string& line : result.log)
        red = red || line.starts_with("[RED] function patch 'no such function' skipped");
    EXPECT_TRUE(red);
}

TEST(RewriteTest, CodeItDoesNotHaveStopsTheRewrite) {
    // A build whose frame step is code in nxnative's patches.py that nxmac has no port of
    const absl::StatusOr<Module> module = ReadModule(Fixture("sys.nso"));
    ASSERT_TRUE(module.ok()) << module.status();
    const absl::StatusOr<PatchTables> tables =
        PatchTables::Parse(absl::StrCat("frame_step\t", module->BuildId(), "\t0100000000001000\n"));
    ASSERT_TRUE(tables.ok()) << tables.status();
    RewriteOptions options;
    options.name = "main";
    options.tables = &*tables;
    const absl::StatusOr<RewriteResult> result = Rewrite(*module, options);
    ASSERT_FALSE(result.ok());
    EXPECT_THAT(std::string(result.status().message()), HasSubstr("0100000000001000"));
    EXPECT_THAT(std::string(result.status().message()), HasSubstr("nxmac does not have yet"));
    // A cut whose original bytes are not the module's
    const absl::StatusOr<PatchTables> cut = PatchTables::Parse(
        absl::StrCat("cut\t", module->BuildId(), "\t0x10\tdeadbeef\t1f2003d5\tnot this build's bytes\n"));
    ASSERT_TRUE(cut.ok()) << cut.status();
    options.tables = &*cut;
    const absl::StatusOr<RewriteResult> mismatch = Rewrite(*module, options);
    ASSERT_FALSE(mismatch.ok());
    EXPECT_THAT(std::string(mismatch.status().message()), HasSubstr("cut original bytes mismatch 0x10"));
}

}  // namespace
}  // namespace nxmac::rewriter
