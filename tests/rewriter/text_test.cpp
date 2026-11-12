#include "text.h"

#include <string_view>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace nxmac::rewriter::internal {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

TEST(TextTest, HexIsPythons) {
    EXPECT_EQ(Hex(std::int64_t{0}), "0x0");
    EXPECT_EQ(Hex(std::int64_t{0x1a}), "0x1a");
    EXPECT_EQ(Hex(std::int64_t{-0x2000000}), "-0x2000000");
    EXPECT_EQ(Hex(std::uint64_t{0x8004000}), "0x8004000");
    EXPECT_EQ(HexBare(0x2d944), "2d944");
    EXPECT_EQ(Hex8(0x1f), "0000001f");
}

TEST(TextTest, BytesAndHexDigits) {
    const std::uint8_t bytes[] = {0x1f, 0x20, 0x03, 0xd5};
    EXPECT_EQ(BytesHex(bytes, 4), "1f2003d5");
    EXPECT_THAT(*HexBytes("1f2003d5"), ElementsAre(0x1f, 0x20, 0x03, 0xd5));
    EXPECT_THAT(*HexBytes(""), IsEmpty());
    EXPECT_FALSE(HexBytes("1f2"));
    EXPECT_FALSE(HexBytes("1g"));
}

TEST(TextTest, ScanMatchesTheWholeText) {
    std::string_view a, b, c;
    EXPECT_TRUE(Scan("x18, x17, x15", "x18, %x, %x", {&a, &b}));
    EXPECT_EQ(a, "x17");
    EXPECT_EQ(b, "x15");
    EXPECT_FALSE(Scan("x18, x17, x15, lsl #2", "x18, %x, %x"));
    EXPECT_FALSE(Scan("x18, w17, x15", "x18, %x, %x"));
    EXPECT_TRUE(Scan("w0, #0x31", "%w, #%i", {&a, &b}));
    EXPECT_EQ(a, "w0");
    EXPECT_EQ(b, "0x31");
    EXPECT_TRUE(Scan("x9, #4", "%r, #%i", {&a, &b}));
    EXPECT_FALSE(Scan("x9, #4", "%w, #%i"));
    EXPECT_FALSE(Scan("w0, #-1", "%w, #%i"));
    EXPECT_FALSE(Scan("w0, #1, lsl #12", "%w, #%i"));
    EXPECT_TRUE(Scan("x18, [x4, x18, lsl #2]", "x18, [%x, x%n, lsl #2]", {&a, &b}));
    EXPECT_EQ(a, "x4");
    EXPECT_EQ(b, "18");
    EXPECT_TRUE(Scan("x5, x3, #0xffffffff", "%r, %r, #%i", {&a, &b, &c}));
    EXPECT_EQ(c, "0xffffffff");
    // Fewer captures than placeholders, and none
    EXPECT_TRUE(Scan("w1, w2", "%w, %w", {&a}));
    EXPECT_EQ(a, "w1");
    EXPECT_TRUE(Scan("w1, w2", "%w, %w"));
    // A register name needs its digits; sp and zr are none
    EXPECT_FALSE(Scan("sp", "%x"));
    EXPECT_FALSE(Scan("x", "%x"));
    EXPECT_FALSE(Scan("xzr", "%x"));
}

TEST(TextTest, ParseIntReadsWhatCapstonePrints) {
    EXPECT_EQ(ParseInt("0x1f"), 0x1f);
    EXPECT_EQ(ParseInt("12"), 12);
    EXPECT_EQ(ParseInt("-0x10"), -16);
    EXPECT_EQ(ParseInt("0"), 0);
    EXPECT_EQ(ParseInt("0xffffffff"), 0xffffffff);
}

TEST(TextTest, RegisterNumbers) {
    EXPECT_EQ(RegNum("x17"), 17);
    EXPECT_EQ(RegNum("w3"), 3);
    EXPECT_EQ(RegNum("sp"), std::nullopt);
    EXPECT_EQ(RegNum("xzr"), std::nullopt);
    EXPECT_EQ(RegNum("lr"), std::nullopt);
    EXPECT_EQ(RegNum("v18"), std::nullopt);
    EXPECT_EQ(RegNum(""), std::nullopt);
}

TEST(TextTest, FindsX18BetweenWordBoundaries) {
    EXPECT_TRUE(Has18("x18, x18, #1"));
    EXPECT_TRUE(Has18("w0, [x1, w18, uxtw #2]"));
    EXPECT_TRUE(Has18("x18"));
    EXPECT_FALSE(Has18("x0, #0x18"));
    EXPECT_FALSE(Has18("v18.16b, v1.16b"));
    EXPECT_FALSE(Has18("d18, x0"));
    EXPECT_FALSE(Has18("x180, x1"));
    EXPECT_FALSE(Has18("w1, #18"));
    EXPECT_THAT(RegsNamed("x18, [x4, w3, uxtw #2]"), ElementsAre(18, 4, 3));
    EXPECT_THAT(RegsNamed("sp, #0x18"), IsEmpty());
    EXPECT_EQ(Subst18("x18, [x18, w18, uxtw #2]", 0), "x0, [x0, w0, uxtw #2]");
    EXPECT_EQ(Subst18("x1, #0x18", 3), "x1, #0x18");
}

}  // namespace
}  // namespace nxmac::rewriter::internal
