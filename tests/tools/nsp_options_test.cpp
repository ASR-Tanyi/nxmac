#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nsp/options.h"

namespace nxmac::nsp {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

absl::StatusOr<Options> Parse(const std::vector<std::string>& args) { return ParseOptions(args); }

TEST(NspOptionsTest, ReadsEachCommandsOptions) {
    absl::StatusOr<Options> o = Parse({"extract", "-k", "prod.keys", "game.nsp", "update.nsp", "-o", "out", "-j", "4"});
    ASSERT_TRUE(o.ok()) << o.status();
    EXPECT_EQ(o->command, "extract");
    EXPECT_EQ(o->keys, "prod.keys");
    EXPECT_EQ(o->out, "out");
    EXPECT_EQ(o->jobs, 4);
    EXPECT_THAT(o->packages, ElementsAre("game.nsp", "update.nsp"));
    EXPECT_FALSE(o->romfs_dir);

    o = Parse({"extract", "--keys=k", "--out=o", "--jobs=2", "--program", "0100000000010001", "--romfs-dir", "g.nsp"});
    ASSERT_TRUE(o.ok()) << o.status();
    EXPECT_EQ(o->keys, "k");
    EXPECT_EQ(o->jobs, 2);
    ASSERT_TRUE(o->program.has_value());
    EXPECT_EQ(o->program->value(), 0x0100000000010001u);
    EXPECT_TRUE(o->romfs_dir);

    o = Parse({"verify", "a.nsp", "b.nsp", "--", "-odd name.nsp"});
    ASSERT_TRUE(o.ok()) << o.status();
    EXPECT_THAT(o->packages, ElementsAre("a.nsp", "b.nsp", "-odd name.nsp"));

    EXPECT_EQ(Parse({"--help"})->command, "help");
    EXPECT_EQ(Parse({"info", "-h"})->command, "help");
}

TEST(NspOptionsTest, SaysWhatIsWrong) {
    EXPECT_THAT(Parse({}).status().message(), HasSubstr("no command"));
    EXPECT_THAT(Parse({"unpack", "a"}).status().message(), HasSubstr("unknown command"));
    EXPECT_THAT(Parse({"info"}).status().message(), HasSubstr("at least one package"));
    EXPECT_THAT(Parse({"extract", "a.nsp"}).status().message(), HasSubstr("needs -o"));
    EXPECT_THAT(Parse({"extract", "a", "b", "c", "-o", "o"}).status().message(), HasSubstr("two packages at most"));
    EXPECT_THAT(Parse({"info", "a", "-o", "o"}).status().message(), HasSubstr("takes no -o"));
    EXPECT_THAT(Parse({"dlc", "a", "-o", "o", "--program", "0100000000010000"}).status().message(),
                HasSubstr("extract's"));
    EXPECT_THAT(Parse({"info", "a", "-j", "0"}).status().message(), HasSubstr("1 to 256"));
    EXPECT_THAT(Parse({"info", "a", "-j", "x"}).status().message(), HasSubstr("1 to 256"));
    EXPECT_THAT(Parse({"info", "a", "-k"}).status().message(), HasSubstr("needs a value"));
    EXPECT_THAT(Parse({"info", "a", "--frobnicate"}).status().message(), HasSubstr("unknown option"));
    EXPECT_THAT(Parse({"extract", "a", "-o", "o", "--program", "zz"}).status().message(), HasSubstr("16 hex digits"));
}

}  // namespace
}  // namespace nxmac::nsp
