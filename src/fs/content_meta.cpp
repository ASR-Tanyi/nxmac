#include "nxmac/fs/content_meta.h"

#include <algorithm>
#include <string_view>

#include "absl/strings/escaping.h"
#include "absl/strings/str_format.h"
#include "bytes.h"

namespace nxmac::fs {

absl::StatusOr<ContentMeta> ParseContentMeta(std::span<const std::uint8_t> bytes) {
    using internal::LoadLe;
    // Header: title ID, version, type, reserved, extended header size, content count, content meta count, and so on
    constexpr std::size_t kHeaderSize = 0x20;
    constexpr std::size_t kContentInfoSize =
        0x38;  // the content's SHA-256, then its ID, size (5 bytes), attributes, type
    if (bytes.size() < kHeaderSize) return absl::InvalidArgumentError("too small for a content meta");
    ContentMeta meta;
    meta.title_id = core::TitleId(LoadLe<std::uint64_t>(bytes, 0));
    meta.version = LoadLe<std::uint32_t>(bytes, 8);
    meta.type = static_cast<ContentMetaType>(bytes[0xc]);
    const auto extended_size = LoadLe<std::uint16_t>(bytes, 0xe);
    const auto count = LoadLe<std::uint16_t>(bytes, 0x10);
    const std::size_t first = kHeaderSize + extended_size;
    if (first + count * kContentInfoSize > bytes.size()) {
        return absl::InvalidArgumentError(absl::StrFormat("content meta of %u contents ends early", count));
    }
    meta.contents.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = first + i * kContentInfoSize + 0x20;
        ContentInfo info;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), info.id.size(), info.id.begin());
        info.size = LoadLe<std::uint32_t>(bytes, at + 0x10) | static_cast<std::uint64_t>(bytes[at + 0x14]) << 32;
        info.type = static_cast<ContentType>(bytes[at + 0x16]);
        info.id_offset = bytes[at + 0x17];
        meta.contents.push_back(info);
    }
    return meta;
}

std::string ContentIdString(const ContentId& id) {
    return absl::BytesToHexString(std::string_view(reinterpret_cast<const char*>(id.data()), id.size()));
}

}  // namespace nxmac::fs
