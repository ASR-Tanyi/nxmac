#include "nxmac/rewriter/patches.h"

#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace nxmac::rewriter {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

constexpr char kBuild[] = "0123456789abcdef0123456789abcdef";
constexpr char kOther[] = "ffffffffffffffffffffffffffffffff";

std::string Rows(std::initializer_list<std::string> rows) {
    std::string text = "# a comment\n\n";
    for (const std::string& row : rows) text += row + "\n";
    return text;
}

TEST(PatchTablesTest, ParsesEveryRow) {
    const std::string fp(64, 'a');
    const absl::StatusOr<PatchTables> tables = PatchTables::Parse(Rows({
        "source\tnxnative 1234",
        std::string("cuts\t") + kBuild,
        std::string("cut\t") + kBuild +
            "\t0x112670c\t6041201e\t4041201e\tlower bound: fmov s0,s11 \xE2\x86\x92 fmov s0,s10",
        std::string("cut\t") + kBuild + "\t0x10\t1f2003d51f2003d5\t0000000000000000\ttwo words",
        std::string("cuts\t") + kOther,
        std::string("obs\t") + kBuild + "\t0x40\t2071\tf9000268\ta gate",
        std::string("capability\t") + kBuild + "\tDYNAMIC_RESOLUTION",
        std::string("frame_step\t") + kBuild + "\t01007EF00011E000",
        std::string("stubs\t") + kOther + "\t0100000000001000",
        "match\t01007EF00011E000\tmain\tone\ta patch",
        "function\t528\t" + fp,
        "fcut\t0x120\t080140b9\t08fddf88\tldr \xE2\x86\x92 ldar",
        "match\t*\tsdk\tseveral\tan SDK patch",
        "function\t64\t" + fp + "," + std::string(64, 'b'),
        "fcut\t0x8\t1f2003d5\t1f2003d5\tone",
        "function\t128\t" + fp,
        "fcut\t0x0\t1f2003d5\t1f2003d5\ttwo",
        "match_frame_step\t0100000000001000\tmain\ta frame step",
        std::string("build\t01007EF00011E000\tv1114112\t1.9.0\tmain=") + kBuild + "\tsdk=" + kOther,
    }));
    ASSERT_TRUE(tables.ok()) << tables.status();
    EXPECT_EQ(tables->source(), "nxnative 1234");
    ASSERT_NE(tables->Cuts(kBuild), nullptr);
    ASSERT_EQ(tables->Cuts(kBuild)->size(), 2u);
    EXPECT_EQ((*tables->Cuts(kBuild))[0].offset, 0x112670c);
    EXPECT_EQ((*tables->Cuts(kBuild))[0].original, "6041201e");
    EXPECT_EQ((*tables->Cuts(kBuild))[0].replacement, "4041201e");
    EXPECT_THAT((*tables->Cuts(kBuild))[0].reason, HasSubstr("lower bound"));
    // A build may register no cuts at all: it keeps its own code, and function patches leave it alone
    ASSERT_NE(tables->Cuts(kOther), nullptr);
    EXPECT_TRUE(tables->Cuts(kOther)->empty());
    EXPECT_EQ(tables->Cuts("00000000000000000000000000000000"), nullptr);
    ASSERT_NE(tables->Obs(kBuild), nullptr);
    EXPECT_EQ((*tables->Obs(kBuild))[0].number, 2071u);
    EXPECT_EQ((*tables->Obs(kBuild))[0].word, "f9000268");
    EXPECT_THAT(*tables->Capabilities(kBuild), ElementsAre("DYNAMIC_RESOLUTION"));
    EXPECT_EQ(*tables->FrameStep(kBuild), "01007EF00011E000");
    EXPECT_EQ(tables->FrameStep(kOther), nullptr);
    EXPECT_EQ(*tables->Stubs(kOther), "0100000000001000");
    const std::vector<MatchRule>* own = tables->ProfileMatchCuts("01007EF00011E000", "main");
    ASSERT_NE(own, nullptr);
    ASSERT_EQ(own->size(), 1u);
    EXPECT_EQ((*own)[0].name, "a patch");
    EXPECT_FALSE((*own)[0].several);
    EXPECT_EQ((*own)[0].functions[0].size, 528);
    EXPECT_EQ((*own)[0].functions[0].cuts[0].offset, 0x120);
    const std::vector<MatchRule>* shared = tables->SharedMatchCuts("sdk");
    ASSERT_NE(shared, nullptr);
    EXPECT_TRUE((*shared)[0].several);
    ASSERT_EQ((*shared)[0].functions.size(), 2u);
    EXPECT_EQ((*shared)[0].functions[0].fingerprints.size(), 2u);
    EXPECT_EQ(tables->SharedMatchCuts("main"), nullptr);
    EXPECT_EQ(tables->ProfileMatchCuts("01007EF00011E000", "sdk"), nullptr);
    EXPECT_THAT(*tables->MatchFrameStep("0100000000001000", "main"), ElementsAre("a frame step"));
    const std::vector<ProfileBuild>* builds = tables->Builds("01007EF00011E000");
    ASSERT_NE(builds, nullptr);
    EXPECT_EQ((*builds)[0].version, "v1114112");
    EXPECT_EQ((*builds)[0].display, "1.9.0");
    EXPECT_THAT((*builds)[0].modules, ElementsAre(std::pair<std::string, std::string>("main", kBuild),
                                                  std::pair<std::string, std::string>("sdk", kOther)));
}

TEST(PatchTablesTest, NamesTheLineThatIsWrong) {
    const auto error = [](std::initializer_list<std::string> rows) {
        return std::string(PatchTables::Parse(Rows(rows)).status().message());
    };
    EXPECT_THAT(error({"cut\tnot-a-build\t0x10\t00\t00\twhy"}), HasSubstr("line 3 (cut)"));
    EXPECT_THAT(error({std::string("cut\t") + kBuild + "\t0x10\t1f2003d5\t1f20\twhy"}), HasSubstr("of one length"));
    EXPECT_THAT(error({std::string("cut\t") + kBuild + "\t0x10\t1f2003zz\t1f2003d5\twhy"}), HasSubstr("line 3"));
    EXPECT_THAT(error({"fcut\t0x0\t1f2003d5\t1f2003d5\tno function before it"}), HasSubstr("line 3 (fcut)"));
    EXPECT_THAT(error({"match\t*\tsdk\tone\ta patch"}), HasSubstr("has no functions or cuts"));
    EXPECT_THAT(error({"something\telse"}), HasSubstr("not a row of the patch tables"));
}

TEST(PatchTablesTest, TheBuiltInTablesHoldBreathOfTheWild) {
    const PatchTables& tables = PatchTables::BuiltIn();
    EXPECT_THAT(tables.source(), HasSubstr("nxnative"));
    EXPECT_GT(tables.build_count(), 10u);
    // 1.9.0's main: its 60 fps cuts and its frame step; its sdk takes the SDK-wide function patches
    constexpr char kMain[] = "cd57b23fa4bbad65803d9788c01821ee";
    ASSERT_NE(tables.Cuts(kMain), nullptr);
    EXPECT_EQ(tables.Cuts(kMain)->size(), 7u);
    ASSERT_NE(tables.FrameStep(kMain), nullptr);
    EXPECT_EQ(*tables.FrameStep(kMain), "01007EF00011E000");
    EXPECT_NE(tables.SharedMatchCuts("sdk"), nullptr);
    const std::vector<ProfileBuild>* builds = tables.Builds("01007EF00011E000");
    ASSERT_NE(builds, nullptr);
    EXPECT_EQ((*builds)[0].modules[0], (std::pair<std::string, std::string>("main", kMain)));
}

}  // namespace
}  // namespace nxmac::rewriter
