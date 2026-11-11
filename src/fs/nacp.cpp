#include "nxmac/fs/nacp.h"

#include <algorithm>
#include <vector>

#include "bytes.h"
#include "zlib.h"

namespace nxmac::fs {
namespace {

constexpr std::size_t kLanguageSize = 0x300;  // a name (0x200), then a publisher (0x100)
constexpr std::size_t kNameSize = 0x200;
constexpr std::size_t kPublisherSize = 0x100;
constexpr std::size_t kTitlesSize = 16 * kLanguageSize;
constexpr std::size_t kDisplayVersion = 0x3060;
constexpr std::size_t kDisplayVersionSize = 0x10;

// The text up to its first NUL
std::string Text(std::span<const std::uint8_t> bytes) {
    return {bytes.begin(), std::find(bytes.begin(), bytes.end(), std::uint8_t{0})};
}

// The 32 languages of a compressed title table: a u16 size, then that much raw deflate data; empty when the bytes are
// not one (an uncompressed table whose first name only looks like a size)
std::vector<std::uint8_t> Inflate(std::span<const std::uint8_t> titles) {
    const auto size = internal::LoadLe<std::uint16_t>(titles, 0);
    if (size == 0 || size > titles.size() - 2) return {};
    std::vector<std::uint8_t> out(32 * kLanguageSize + 1);
    z_stream z{};
    if (inflateInit2(&z, -15) != Z_OK) return {};
    z.next_in = const_cast<Bytef*>(titles.data() + 2);  // zlib's API is not const-correct; it does not write the input
    z.avail_in = size;
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    const int rc = inflate(&z, Z_FINISH);
    const std::size_t produced = out.size() - z.avail_out;
    inflateEnd(&z);
    if (rc != Z_STREAM_END || produced != 32 * kLanguageSize) return {};
    out.resize(produced);
    return out;
}

}  // namespace

absl::StatusOr<ControlData> ParseNacp(std::span<const std::uint8_t> nacp) {
    if (nacp.size() < kDisplayVersion + kDisplayVersionSize) return absl::InvalidArgumentError("too small for a NACP");
    const std::span<const std::uint8_t> plain = nacp.first(kTitlesSize);
    const std::vector<std::uint8_t> inflated = Inflate(plain);
    const std::span<const std::uint8_t> titles = inflated.empty() ? plain : std::span<const std::uint8_t>(inflated);
    ControlData data;
    for (std::size_t at = 0; at + kLanguageSize <= titles.size(); at += kLanguageSize) {
        std::string name = Text(titles.subspan(at, kNameSize));
        if (name.empty()) continue;
        data.name = std::move(name);
        data.publisher = Text(titles.subspan(at + kNameSize, kPublisherSize));
        break;
    }
    data.display_version = Text(nacp.subspan(kDisplayVersion, kDisplayVersionSize));
    return data;
}

}  // namespace nxmac::fs
