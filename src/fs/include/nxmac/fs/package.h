// An NSP opened for its content: the partition filesystem's common tickets give the title keys, and each .nca file is
// opened with them.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "nxmac/core/title_id.h"
#include "nxmac/fs/content_meta.h"
#include "nxmac/fs/keys.h"
#include "nxmac/fs/nca.h"
#include "nxmac/fs/storage.h"

namespace nxmac::fs {

class Package {
public:
    struct Content {
        std::string file_name;   // as in the package: "<content ID>.nca"
        std::string content_id;  // the name before the first ".", lower case
        Nca nca;
    };

    // The common tickets are added to a copy of keys; a personalized ticket is skipped (its NCAs report the missing
    // title key). An NCA that does not open (a damaged header, another console family's header_key) fails Open, which
    // names its file.
    static absl::StatusOr<Package> Open(const KeySet& keys, StoragePtr storage);
    static absl::StatusOr<Package> OpenFile(const KeySet& keys, const std::string& path);

    const std::vector<Content>& contents() const { return contents_; }
    // The NCA of this content type and program ID, or of the lowest program ID when none is given; nullptr when there
    // is none
    const Nca* Find(NcaContentType type, std::optional<core::TitleId> program_id = std::nullopt) const;
    // The content metas of its Meta NCAs, in package order
    absl::StatusOr<std::vector<ContentMeta>> ContentMetas() const;

private:
    std::vector<Content> contents_;
};

}  // namespace nxmac::fs
