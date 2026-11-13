// shader's command line, parsed.
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "shader/options.h"

namespace nxmac::shader_tool {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

absl::StatusOr<Options> Parse(const std::vector<std::string>& args) { return ParseOptions(args); }

TEST(ShaderOptionsTest, Split) {
    const absl::StatusOr<Options> o =
        Parse({"split", "-g", "01007EF00011E000", "-o", "out", "--main", "v0/main", "v0/RomFS.img", "--main=v1/main",
               "v1/RomFS", "--dlc", "aoc/1", "-j", "4", "--glslc-capture", "cap", "-p", "p.tsv"});
    ASSERT_TRUE(o.ok()) << o.status();
    EXPECT_EQ(o->command, "split");
    EXPECT_EQ(o->game, "01007EF00011E000");
    EXPECT_EQ(o->out, "out");
    EXPECT_THAT(o->romfs, ElementsAre("v0/RomFS.img", "v1/RomFS"));
    EXPECT_THAT(o->mains, ElementsAre("v0/main", "v1/main"));
    EXPECT_THAT(o->dlc, ElementsAre("aoc/1"));
    EXPECT_EQ(o->jobs, 4);
    EXPECT_EQ(o->glslc_capture, "cap");
    EXPECT_EQ(o->profiles, "p.tsv");
}

TEST(ShaderOptionsTest, Build) {
    const absl::StatusOr<Options> o =
        Parse({"build", "-g", "01007EF00011E000", "--nxnative", "../nxnative", "-o", "out", "--tables", "t",
               "--cache=c", "--min-macos", "none", "-j", "6", "w/shader/pkg"});
    ASSERT_TRUE(o.ok()) << o.status();
    EXPECT_EQ(o->command, "build");
    EXPECT_THAT(o->romfs, ElementsAre("w/shader/pkg"));
    EXPECT_EQ(o->nxnative, "../nxnative");
    EXPECT_EQ(o->tables, "t");
    EXPECT_EQ(o->cache, "c");
    EXPECT_EQ(o->min_macos, "");
    EXPECT_EQ(o->jobs, 6);
    const absl::StatusOr<Options> p =
        Parse({"build", "--profile", "prof", "--xlate", "x/dmsh_xlate", "-o", "out", "pkg"});
    ASSERT_TRUE(p.ok()) << p.status();
    EXPECT_EQ(p->profile, "prof");
    EXPECT_EQ(p->xlate, "x/dmsh_xlate");
    EXPECT_EQ(p->min_macos, "15.0");
}

TEST(ShaderOptionsTest, ProfileAndHelp) {
    const absl::StatusOr<Options> p = Parse({"profile", "01007EF00011E000", "0100000000010000"});
    ASSERT_TRUE(p.ok()) << p.status();
    EXPECT_THAT(p->romfs, ElementsAre("01007EF00011E000", "0100000000010000"));
    EXPECT_EQ(Parse({"--help"})->command, "help");
    EXPECT_EQ(Parse({"split", "-h"})->command, "help");
}

TEST(ShaderOptionsTest, Refuses) {
    const std::vector<std::pair<std::vector<std::string>, std::string>> bad = {
        {{}, "no command"},
        {{"extract"}, "unknown command"},
        {{"split", "-o", "o", "--main", "m", "r"}, "-g"},
        {{"split", "-g", "x", "--main", "m", "r"}, "-o"},
        {{"split", "-g", "x", "-o", "o"}, "a RomFS"},
        {{"split", "-g", "x", "-o", "o", "r"}, "1 RomFS and 0 --main"},
        {{"split", "-g", "x", "-o", "o", "--main", "a", "--main", "b", "r"}, "2 --main"},
        {{"split", "-g"}, "needs a value"},
        {{"split", "-j", "0"}, "from 1 to 256"},
        {{"split", "--bogus"}, "unknown option"},
        {{"profile"}, "at least one title ID"},
        {{"profile", "x", "-o", "o"}, "only -p"},
        {{"build", "--profile", "p", "--xlate", "x", "pkg"}, "-o <dir>"},
        {{"build", "--profile", "p", "--xlate", "x", "-o", "o"}, "one package folder"},
        {{"build", "--profile", "p", "--xlate", "x", "-o", "o", "a", "b"}, "one package folder"},
        {{"build", "--xlate", "x", "-o", "o", "pkg"}, "--profile <dir>, or -g <id> with --nxnative <dir>"},
        {{"build", "-g", "x", "--xlate", "x", "-o", "o", "pkg"}, "--profile <dir>, or -g <id> with --nxnative <dir>"},
        {{"build", "--profile", "p", "-o", "o", "pkg"}, "--xlate <file>, or --nxnative <dir>"},
        {{"build", "--profile", "p", "--xlate", "x", "-o", "o", "--main", "m", "pkg"}, "takes no --main"},
        {{"split", "-g", "x", "-o", "o", "--main", "m", "--xlate", "x", "r"}, "takes no build options"},
    };
    for (const auto& [args, why] : bad) {
        const absl::StatusOr<Options> o = Parse(args);
        ASSERT_FALSE(o.ok()) << args.size();
        EXPECT_THAT(std::string(o.status().message()), HasSubstr(why));
    }
}

}  // namespace
}  // namespace nxmac::shader_tool
