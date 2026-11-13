// The regular expression layer of the shader build (src/dmsh/re.h) against what Python's re gives for the same calls
// (the expected values are Python 3.13's).
#include "re.h"

#include <string>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace nxmac::dmsh::re {
namespace {

using ::testing::ElementsAre;

TEST(ReTest, SubStepsAsPythonDoes) {
    EXPECT_EQ(sub("x*", "-", "abxd"), "-a-b--d-");                          // an empty match beside one that was not
    EXPECT_EQ(sub(R"((a)|(b))", R"([\1|\2|\g<1>])", "ab"), "[a||a][|b|]");  // a group that took no part gives nothing
    EXPECT_EQ(sub(R"((?P<n>\d+))", R"(<\g<n>>)", "a12b3"), "a<12>b<3>");
    EXPECT_EQ(
        sub(
            R"(\d)", [](const Match& m) { return std::to_string(std::stoi(std::string(m.group())) * 2); }, "a12", 1),
        "a22");
    std::size_t n = 0;
    EXPECT_EQ(Pattern("a").sub("b", "aXa", 0, &n), "bXb");
    EXPECT_EQ(n, 2u);
}

TEST(ReTest, LineAnchors) {
    EXPECT_EQ(sub("^", ">", "a\nb\n", 0, M), ">a\n>b\n>");  // after the newline that ends the text too
    EXPECT_EQ(sub("$", "<", "a\nb\n"), "a\nb<\n<");
    EXPECT_EQ(sub("$", "<", "a\nb\n", 0, M), "a<\nb<\n<");
    EXPECT_FALSE(search(R"(a\Z)", "a\n"));  // \Z is the very end
    EXPECT_TRUE(search("a$", "a\n"));
}

TEST(ReTest, FinditerAndFindall) {
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    for (const Match& m : Pattern(R"(\b)").finditer("ab cd")) spans.emplace_back(m.start(), m.end());
    EXPECT_THAT(spans, ElementsAre(std::pair(0u, 0u), std::pair(2u, 2u), std::pair(3u, 3u), std::pair(5u, 5u)));
    EXPECT_THAT(Pattern(R"(\w+)").findall("ab cd"), ElementsAre("ab", "cd"));
    EXPECT_THAT(Pattern(R"((\w)\w)").findall("ab cd"), ElementsAre("a", "c"));
    EXPECT_THAT(Pattern("(,)|;").split("a,b;c"), ElementsAre("a", ",", "b", "", "c"));
}

TEST(ReTest, PositionsSeeTheWholeText) {
    const Pattern b("b");
    EXPECT_EQ(b.search("abcb", 2)->start(), 3u);
    EXPECT_FALSE(Pattern("^b").search("ab", 1));                 // ^ is the text's start, not the position
    EXPECT_EQ(Pattern("(?<=a)b").search("ab", 1)->start(), 1u);  // a lookbehind sees what comes before it
    EXPECT_FALSE(match("b", "ab"));
    EXPECT_EQ(b.match("ab", 1)->start(), 1u);
    EXPECT_EQ(fullmatch("a|ab", "ab")->group(), "ab");
    EXPECT_FALSE(b.search("abc", 0, 1));  // endpos cuts the text
}

TEST(ReTest, ClassesAsPythonReadsThem) {
    EXPECT_EQ(search(R"([[\]x]+)", "a[x]]b")->group(), "[x]]");  // a [ in a class is itself
    EXPECT_EQ(search("[^[]+", "ab[c")->group(), "ab");
    EXPECT_EQ(search("[]a]+", "x]a]")->group(), "]a]");   // a ] that comes first is a member
    EXPECT_EQ(search("[[:x]+", "a:[x")->group(), ":[x");  // no POSIX classes in Python
}

TEST(ReTest, GroupsAndNames) {
    const Pattern p(R"((?P<a>x)?(y))");
    const auto m = p.search("zy");
    ASSERT_TRUE(m);
    EXPECT_FALSE(m->has(1));
    EXPECT_FALSE(m->has("a"));
    EXPECT_EQ(m->group(2), "y");
    EXPECT_EQ(m->groups(), 2);
    EXPECT_EQ(p.group_number("a"), 1);
    EXPECT_EQ(p.group_number("nope"), -1);
    EXPECT_THROW(Pattern("("), std::invalid_argument);
}

TEST(ReTest, Escape) { EXPECT_EQ(escape("a.b*c [d] {e} #f-g&h~i"), R"(a\.b\*c\ \[d\]\ \{e\}\ \#f\-g\&h\~i)"); }

TEST(ReTest, LongMatchesHaveNoLimit) {
    // a lazy scan over a megabyte, and deep backtracking: neither stops
    const std::string big = std::string(1 << 20, 'a') + "b";
    EXPECT_EQ(Pattern("(?s)^(.*?)b$").search(big)->group(1).size(), std::size_t(1) << 20);
    EXPECT_EQ(Pattern("(?:a|a)+b").search(big)->end(), big.size());
}

}  // namespace
}  // namespace nxmac::dmsh::re
