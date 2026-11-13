#include "nxmac/loader/sdk.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace nxmac::loader {
namespace {

// Module data: the strings among zeros and other bytes, as an sdk module's read-only data has them
std::vector<std::uint8_t> Data(const std::vector<std::string>& strings) {
    std::vector<std::uint8_t> d(64, 0x5a);
    for (const std::string& s : strings) {
        d.insert(d.end(), s.begin(), s.end());
        d.insert(d.end(), {0, 0, 0, 0x41});
    }
    return d;
}

TEST(SdkTest, TheVersionString) {
    const SdkVersion v = FindSdkVersion(Data({"SDK MW+NintendoSDK_nnSdk-20.5.22-Release", "nnSdk-1.2.3"}));
    EXPECT_EQ(v.major, 20);
    EXPECT_EQ(v.text, "20.5.22-Release");
}

TEST(SdkTest, AVariantNameAndAnyCase) {
    const SdkVersion v = FindSdkVersion(Data({"NINTENDOSDK_nnSdk-Generic_16.2.4-Release_with_a_long_tail"}));
    EXPECT_EQ(v.major, 16);
    EXPECT_EQ(v.text, "Generic_16.2.4-Relea");  // at most 20 characters
}

TEST(SdkTest, OnlyAfterNintendoSdkAndWithADigit) {
    EXPECT_EQ(FindSdkVersion(Data({"_nnSdk-20.5.22"})).major, 0);              // nothing before it
    EXPECT_EQ(FindSdkVersion(Data({"OtherVendorX_nnSdk-20.5.22"})).major, 0);  // someone else's
    EXPECT_EQ(FindSdkVersion(Data({"NintendoSDK_nnSdk-Release"})).major, 0);   // no number
    // The first one that is a version, past those that are not
    const SdkVersion v = FindSdkVersion(Data({"NintendoSDK_nnSdk-Release", "NintendoSDK_nnSdk-9.3.0"}));
    EXPECT_EQ(v.major, 9);
    EXPECT_EQ(FindSdkVersion(Data({})).major, 0);
    EXPECT_EQ(FindSdkVersion({}).major, 0);
}

TEST(SdkTest, TheSwitch2Build) {
    EXPECT_TRUE(IsOunce(Data({"FS_ACCESS: { sdk_version: 20.5.19, spec: Ounce }"})));
    EXPECT_FALSE(IsOunce(Data({"FS_ACCESS: { sdk_version: 20.5.22, spec: NX }"})));
    EXPECT_FALSE(IsOunce({}));
}

}  // namespace
}  // namespace nxmac::loader
