// The profile table (src/shader/profiles.tsv, built in): its rows, and what it holds for Breath of the Wild.
#include "nxmac/shader/profiles.h"

#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace nxmac::shader {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::Pair;

TEST(ProfilesTest, ParsesEveryRow) {
    const absl::StatusOr<ShaderProfiles> p = ShaderProfiles::Parse(
        "# a comment\n"
        "source\tnxnative abc, 1 profiles\n"
        "profile\t0100000000001000\n"
        "groups\t0100000000001000\t/Shader/=shader\t/Effect/=effect\n"
        "note\t0100000000001000\tA game's shaders\twith a tab\n"
        "key_skip\t0100000000001000\t1904\t120\n"
        "modules\t0100000000001000\tmain\tsubsdk0\tsdk\n"
        "fact\t0100000000001000\t0123456789abcdef\tfout\t{\"0\":\"uint\"}\n"
        "fact\t0100000000001000\t0123456789abcdef\tattr\t[1,2]\n"
        "\n");
    ASSERT_TRUE(p.ok()) << p.status();
    EXPECT_EQ(p->source(), "nxnative abc, 1 profiles");
    const absl::StatusOr<const ShaderProfile*> g = p->Find("0100000000001000");
    ASSERT_TRUE(g.ok());
    EXPECT_THAT((*g)->groups, ElementsAre(Pair("/Shader/", "shader"), Pair("/Effect/", "effect")));
    EXPECT_EQ((*g)->note, "A game's shaders\twith a tab");
    EXPECT_EQ((*g)->key_skip_offset, 1904);
    EXPECT_EQ((*g)->key_skip_length, 120);
    EXPECT_THAT((*g)->modules, ElementsAre("main", "subsdk0", "sdk"));
    EXPECT_THAT((*g)->facts.at("0123456789abcdef"),
                ElementsAre(Pair("attr", "[1,2]"), Pair("fout", "{\"0\":\"uint\"}")));
}

TEST(ProfilesTest, FindsATitleIdInEitherCase) {
    const absl::StatusOr<ShaderProfiles> p =
        ShaderProfiles::Parse("profile\t01007ef00011e000\nnote\t01007EF00011E000\tx\n");
    ASSERT_TRUE(p.ok()) << p.status();
    EXPECT_TRUE(p->Find("01007ef00011E000").ok());
    const absl::StatusOr<const ShaderProfile*> missing = p->Find("0100000000000000");
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.status().code(), absl::StatusCode::kNotFound);
    EXPECT_THAT(std::string(missing.status().message()), HasSubstr("01007EF00011E000"));
}

TEST(ProfilesTest, RefusesWhatItCannotRead) {
    for (const char* text : {"note\t0100000000001000\tbefore its profile\n", "profile\tnot-an-id\n",
                             "profile\t0100000000001000\nprofile\t0100000000001000\n",
                             "profile\t0100000000001000\ngroups\t0100000000001000\tno-equals\n",
                             "profile\t0100000000001000\nkey_skip\t0100000000001000\t-1\t2\n",
                             "profile\t0100000000001000\nfact\t0100000000001000\tk\n",
                             "profile\t0100000000001000\nweird\t0100000000001000\n"}) {
        const absl::StatusOr<ShaderProfiles> p = ShaderProfiles::Parse(text);
        EXPECT_FALSE(p.ok()) << text;
        if (!p.ok()) {
            EXPECT_EQ(p.status().code(), absl::StatusCode::kInvalidArgument);
        }
    }
    EXPECT_EQ(ShaderProfiles::ReadFile("/nonexistent/profiles.tsv").status().code(), absl::StatusCode::kNotFound);
}

TEST(ProfilesTest, BuiltInHoldsBreathOfTheWild) {
    const ShaderProfiles& p = ShaderProfiles::BuiltIn();
    EXPECT_GT(p.size(), 100u);
    EXPECT_THAT(p.source(), HasSubstr("nxnative"));
    const absl::StatusOr<const ShaderProfile*> botw = p.Find("01007EF00011E000");
    ASSERT_TRUE(botw.ok()) << botw.status();
    EXPECT_EQ((*botw)->note, "Breath of the Wild shaders");
    EXPECT_THAT((*botw)->groups,
                ElementsAre(Pair("/Shader/", "shader"), Pair("/Effect/", "effect"), Pair("/Layout/", "layout"),
                            Pair("/System/", "gsys"), Pair("/Terrain/", "terrain")));
    EXPECT_EQ((*botw)->key_skip_length, 0);
    EXPECT_EQ((*botw)->facts.size(), 2u);
}

}  // namespace
}  // namespace nxmac::shader
