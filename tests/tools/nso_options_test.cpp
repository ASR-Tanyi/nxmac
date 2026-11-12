#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nso/options.h"

namespace nxmac::nso {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

std::string Error(const std::vector<std::string>& args) { return std::string(ParseOptions(args).status().message()); }

TEST(NsoOptionsTest, ParsesRewrite) {
    const absl::StatusOr<Options> options = ParseOptions(
        {"rewrite", "-o", "out", "--game=01007EF00011E000", "-j", "3", "--spin-list", "ExeFS/main", "ExeFS/sdk"});
    ASSERT_TRUE(options.ok()) << options.status();
    EXPECT_EQ(options->command, "rewrite");
    EXPECT_EQ(options->out, "out");
    EXPECT_EQ(options->game, "01007EF00011E000");
    EXPECT_EQ(options->jobs, 3);
    EXPECT_TRUE(options->spin_list);
    EXPECT_THAT(options->modules, ElementsAre("ExeFS/main", "ExeFS/sdk"));
}

TEST(NsoOptionsTest, ParsesInfoAndHelp) {
    const absl::StatusOr<Options> info = ParseOptions({"info", "-p", "tables.tsv", "main"});
    ASSERT_TRUE(info.ok()) << info.status();
    EXPECT_EQ(info->patches, "tables.tsv");
    EXPECT_EQ(ParseOptions({"--help"})->command, "help");
    EXPECT_EQ(ParseOptions({"rewrite", "-h"})->command, "help");
    // After --, a name that starts with a dash is a module
    EXPECT_THAT(ParseOptions({"info", "--", "-odd"})->modules, ElementsAre("-odd"));
}

TEST(NsoOptionsTest, SaysWhatIsWrong) {
    EXPECT_THAT(Error({}), HasSubstr("no command"));
    EXPECT_THAT(Error({"patch", "main"}), HasSubstr("unknown command"));
    EXPECT_THAT(Error({"rewrite", "main"}), HasSubstr("needs -o"));
    EXPECT_THAT(Error({"rewrite", "-o", "out"}), HasSubstr("at least one module"));
    EXPECT_THAT(Error({"rewrite", "-o"}), HasSubstr("needs a value"));
    EXPECT_THAT(Error({"rewrite", "-o", "out", "-j", "0", "main"}), HasSubstr("from 1 to 256"));
    EXPECT_THAT(Error({"rewrite", "-o", "out", "-n", "main", "a", "b"}), HasSubstr("one module"));
    EXPECT_THAT(Error({"rewrite", "-o", "out", "-n", "a/b", "a"}), HasSubstr("not a path"));
    EXPECT_THAT(Error({"rewrite", "-o", "out", "--fast", "main"}), HasSubstr("unknown option"));
    EXPECT_THAT(Error({"info", "-o", "out", "main"}), HasSubstr("writes nothing"));
    EXPECT_THAT(Error({"info", "-g", "01007EF00011E000", "main"}), HasSubstr("only -p"));
}

}  // namespace
}  // namespace nxmac::nso
