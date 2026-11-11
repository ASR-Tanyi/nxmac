#include "nxmac/core/title_id.h"

#include <string_view>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace nxmac::core {
namespace {

using ::testing::HasSubstr;

// Breath of the Wild's IDs, as its NSP file names have them
constexpr TitleId kBotw{0x01007EF00011E000};

TEST(TitleIdTest, ParsesSixteenHexDigitsInEitherCase) {
    absl::StatusOr<TitleId> id = TitleId::Parse("01007EF00011E000");
    ASSERT_TRUE(id.ok()) << id.status();
    EXPECT_EQ(*id, kBotw);
    EXPECT_EQ(TitleId::Parse("01007ef00011e000").value(), kBotw);
}

TEST(TitleIdTest, RejectsAnythingElse) {
    for (std::string_view bad : {"", "01007EF00011E00", "01007EF00011E0000", "0x007EF00011E000", "+1007EF00011E000",
                                 " 1007EF00011E000", "01007EF00011E00G"}) {
        EXPECT_EQ(TitleId::Parse(bad).status().code(), absl::StatusCode::kInvalidArgument) << '"' << bad << '"';
    }
    EXPECT_THAT(TitleId::Parse("zelda").status().message(), HasSubstr("\"zelda\""));
}

TEST(TitleIdTest, FormatsAsSixteenUpperCaseDigits) {
    EXPECT_EQ(kBotw.ToString(), "01007EF00011E000");
    EXPECT_EQ(TitleId(1).ToString(), "0000000000000001");
    EXPECT_EQ(absl::StrCat(kBotw), "01007EF00011E000");
}

TEST(TitleIdTest, DerivesTheUpdateAndAddOnContentIds) {
    EXPECT_EQ(kBotw.Update().ToString(), "01007EF00011E800");
    EXPECT_EQ(kBotw.AddOnContent(1).ToString(), "01007EF00011F001");
    EXPECT_EQ(kBotw.AddOnContent(2).ToString(), "01007EF00011F002");
}

}  // namespace
}  // namespace nxmac::core
