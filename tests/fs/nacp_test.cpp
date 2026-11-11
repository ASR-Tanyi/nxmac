#include "nxmac/fs/nacp.h"

#include <cstring>
#include <string>

#include "gtest/gtest.h"
#include "test_builders.h"
#include "zlib.h"

namespace nxmac::fs {
namespace {

using testing::Bytes;

Bytes Nacp() {
    Bytes nacp(0x4000, 0);
    std::memcpy(nacp.data() + 0x3060, "1.9.0", 5);
    return nacp;
}

void Put(Bytes& bytes, std::size_t at, const std::string& text) {
    std::memcpy(bytes.data() + at, text.data(), text.size());
}

TEST(NacpTest, TakesTheFirstLanguageWithAName) {
    Bytes nacp = Nacp();
    Put(nacp, 2 * 0x300, "Le jeu");  // the third language (Japanese's slot) holds the first name
    Put(nacp, 2 * 0x300 + 0x200, "Nintendo");
    Put(nacp, 5 * 0x300, "Another");
    absl::StatusOr<ControlData> data = ParseNacp(nacp);
    ASSERT_TRUE(data.ok()) << data.status();
    EXPECT_EQ(data->name, "Le jeu");
    EXPECT_EQ(data->publisher, "Nintendo");
    EXPECT_EQ(data->display_version, "1.9.0");
}

TEST(NacpTest, InflatesTheCompressedNamesOfNewerSdks) {
    // 32 languages of 0x300 bytes, raw deflate after a u16 size, in place of the 16 plain ones
    Bytes titles(32 * 0x300, 0);
    Put(titles, 20 * 0x300, "Compressed Game");
    Bytes packed(0x3000);
    z_stream z{};
    ASSERT_EQ(deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY), Z_OK);
    z.next_in = titles.data();
    z.avail_in = static_cast<uInt>(titles.size());
    z.next_out = packed.data();
    z.avail_out = static_cast<uInt>(packed.size());
    ASSERT_EQ(deflate(&z, Z_FINISH), Z_STREAM_END);
    const std::size_t size = packed.size() - z.avail_out;
    deflateEnd(&z);

    Bytes nacp = Nacp();
    nacp[0] = static_cast<std::uint8_t>(size);
    nacp[1] = static_cast<std::uint8_t>(size >> 8);
    std::copy(packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(size), nacp.begin() + 2);
    absl::StatusOr<ControlData> data = ParseNacp(nacp);
    ASSERT_TRUE(data.ok()) << data.status();
    EXPECT_EQ(data->name, "Compressed Game");
}

TEST(NacpTest, ANameThatOnlyLooksLikeASizeIsReadPlain) {
    Bytes nacp = Nacp();
    Put(nacp, 0, "AB plain name");  // "AB" reads as a size of 0x4241, which does not inflate
    absl::StatusOr<ControlData> data = ParseNacp(nacp);
    ASSERT_TRUE(data.ok());
    EXPECT_EQ(data->name, "AB plain name");
}

TEST(NacpTest, RejectsWhatIsTooShort) { EXPECT_FALSE(ParseNacp(Bytes(0x100)).ok()); }

}  // namespace
}  // namespace nxmac::fs
