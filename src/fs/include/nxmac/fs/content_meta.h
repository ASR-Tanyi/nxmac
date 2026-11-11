// Content metas (the .cnmt file in a Meta NCA): which title, version and kind a package holds, and the NCAs (contents)
// it is made of, by content ID: the file names of the NCAs in an NSP.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "nxmac/core/title_id.h"

namespace nxmac::fs {

enum class ContentMetaType : std::uint8_t {
    kSystemProgram = 0x01,
    kSystemData = 0x02,
    kSystemUpdate = 0x03,
    kBootImagePackage = 0x04,
    kBootImagePackageSafe = 0x05,
    kApplication = 0x80,
    kPatch = 0x81,
    kAddOnContent = 0x82,
    kDelta = 0x83,
    kDataPatch = 0x84,
};

// A content meta's kinds of content; not the NCA header's content type, which numbers them differently
enum class ContentType : std::uint8_t {
    kMeta = 0,
    kProgram = 1,
    kData = 2,
    kControl = 3,
    kHtmlDocument = 4,
    kLegalInformation = 5,
    kDeltaFragment = 6,
};

using ContentId = std::array<std::uint8_t, 16>;

struct ContentInfo {
    ContentId id{};
    std::uint64_t size = 0;
    ContentType type = ContentType::kMeta;
    std::uint8_t id_offset = 0;  // which program of a multi-program application
};

struct ContentMeta {
    core::TitleId title_id;
    std::uint32_t version = 0;
    ContentMetaType type = ContentMetaType::kApplication;
    std::vector<ContentInfo> contents;  // without the Meta NCA itself
};

// A packaged content meta (.cnmt); InvalidArgument when its tables do not fit in bytes
absl::StatusOr<ContentMeta> ParseContentMeta(std::span<const std::uint8_t> bytes);

// A content ID as an NSP names its NCA file: 32 lower-case hex digits
std::string ContentIdString(const ContentId& id);

}  // namespace nxmac::fs
