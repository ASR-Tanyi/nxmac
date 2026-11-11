#include "nxmac/fs/keys.h"

#include <cstdio>
#include <fstream>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "test_builders.h"

namespace nxmac::fs {
namespace {

using ::testing::AllOf;
using ::testing::HasSubstr;
using ::testing::Not;
using testing::PatternBytes;

TEST(KeySetTest, KeepsTheKeysThatOpenContent) {
    const KeySet keys = testing::TestKeys();
    ASSERT_NE(keys.header_key(), nullptr);
    EXPECT_EQ(testing::ToHex(*keys.header_key()), testing::ToHex(PatternBytes(32, 1)));
    for (int rev = 0; rev < 4; ++rev) {
        ASSERT_NE(keys.key_area_key(KeyAreaKeyKind::kApplication, rev), nullptr) << rev;
        EXPECT_EQ(testing::ToHex(*keys.key_area_key(KeyAreaKeyKind::kApplication, rev)),
                  testing::ToHex(PatternBytes(16, 100 + rev)));
        ASSERT_NE(keys.titlekek(rev), nullptr) << rev;  // written TITLEKEK_0N: names are not case-sensitive
        EXPECT_EQ(testing::ToHex(*keys.titlekek(rev)), testing::ToHex(PatternBytes(16, 200 + rev)));
    }
    EXPECT_EQ(keys.key_area_key(KeyAreaKeyKind::kApplication, 4), nullptr);
    EXPECT_NE(keys.key_area_key(KeyAreaKeyKind::kOcean, 0), nullptr);
    EXPECT_NE(keys.key_area_key(KeyAreaKeyKind::kSystem, 0), nullptr);
    EXPECT_EQ(keys.key_area_key(KeyAreaKeyKind::kSystem, 1), nullptr);
    EXPECT_EQ(keys.titlekek(-1), nullptr);
    EXPECT_EQ(keys.titlekek(KeySet::kRevisions), nullptr);
}

TEST(KeySetTest, SkipsKeysItDoesNotUse) {
    absl::StatusOr<KeySet> keys = KeySet::Parse(
        "eticket_rsa_keypair = 00112233\n"
        "titlekek_source = 00\n"
        "titlekek_zz = 0011\n"
        "key_area_key_application_100 = 00\n");
    ASSERT_TRUE(keys.ok()) << keys.status();
    EXPECT_EQ(keys->header_key(), nullptr);
}

TEST(KeySetTest, AMalformedLineNamesItsKeyButNeverItsValue) {
    const std::string secret = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeZZ";
    absl::StatusOr<KeySet> keys = KeySet::Parse("; comment\nheader_key = " + secret + "\n");
    ASSERT_FALSE(keys.ok());
    EXPECT_EQ(keys.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_THAT(keys.status().message(), AllOf(HasSubstr("line 2"), HasSubstr("header_key"), HasSubstr("64 hex digits"),
                                               Not(HasSubstr("0011"))));

    keys = KeySet::Parse("titlekek_01 = 0011\n");
    ASSERT_FALSE(keys.ok());
    EXPECT_THAT(keys.status().message(), AllOf(HasSubstr("titlekek_01"), Not(HasSubstr("0011"))));

    keys = KeySet::Parse("not a key line\n");
    ASSERT_FALSE(keys.ok());
    EXPECT_THAT(keys.status().message(), HasSubstr("line 1"));
}

TEST(KeySetTest, HoldsTitleKeysByRightsId) {
    KeySet keys;
    const RightsId rights = {0x01, 0x00, 0x7e, 0xf0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0a};
    EXPECT_EQ(keys.encrypted_title_key(rights), nullptr);
    keys.AddTitleKey(rights, Aes128Key{9, 9, 9});
    ASSERT_NE(keys.encrypted_title_key(rights), nullptr);
    EXPECT_EQ((*keys.encrypted_title_key(rights))[0], 9);
}

TEST(KeySetTest, NamesKeysAsProdKeysDoes) {
    EXPECT_EQ(KeySet::KeyAreaKeyName(KeyAreaKeyKind::kApplication, 0x0a), "key_area_key_application_0a");
    EXPECT_EQ(KeySet::KeyAreaKeyName(KeyAreaKeyKind::kSystem, 0x14), "key_area_key_system_14");
    EXPECT_EQ(KeySet::TitlekekName(0x13), "titlekek_13");
}

TEST(KeySetTest, LoadsAFile) {
    const std::string path = ::testing::TempDir() + "nxmac_keys_test.keys";
    std::ofstream(path) << testing::TestKeysText();
    absl::StatusOr<KeySet> keys = KeySet::Load(path);
    ASSERT_TRUE(keys.ok()) << keys.status();
    EXPECT_NE(keys->header_key(), nullptr);
    std::remove(path.c_str());

    EXPECT_EQ(KeySet::Load(path).status().code(), absl::StatusCode::kNotFound);
}

}  // namespace
}  // namespace nxmac::fs
