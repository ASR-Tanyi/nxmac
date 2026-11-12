// A RomFS as the split walks it (internal to nxmac::shader): a folder, or nxnative's DMROMFS1 image read as one
// folder, as dm/tools/romfs.py lets shader_split.py read it. Every file, sorted by its path as os.walk and sorted()
// give them, and its bytes.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "nxmac/fs/romfs_image.h"

namespace nxmac::shader::internal {

class RomFsSource {
public:
    struct File {
        std::string rel;        // the path below the root, '/'-separated, as os.path.relpath gives it
        std::size_t entry = 0;  // an image's entry
    };

    // path: a folder, or a file that is a DMROMFS1 image (one whose first 8 bytes are its magic)
    static absl::StatusOr<std::unique_ptr<RomFsSource>> Open(const std::string& path);

    const std::string& root() const { return root_; }
    bool image() const { return image_.has_value(); }
    // In the order shader_split.py scans them
    const std::vector<File>& files() const { return files_; }
    // The first n bytes of a file (all of it when n is 0); IoError for a file that cannot be read
    absl::StatusOr<std::string> Read(const File& file, std::uint64_t n = 0) const;
    // n bytes at offset (fewer at the end), and a file's size (0 when it cannot be read)
    absl::StatusOr<std::string> ReadAt(const File& file, std::uint64_t offset, std::uint64_t n) const;
    std::uint64_t Size(const File& file) const;
    // Whether rel names a file or a folder (os.path.exists inside the RomFS)
    bool Exists(const std::string& rel) const;

private:
    std::string root_;
    std::optional<fs::RomFsImage> image_;
    std::vector<File> files_;
    std::set<std::string> paths_;  // every file and folder, by rel
};

// Whether the file at path starts with DMROMFS1 (romfs.is_image)
bool IsImage(const std::string& path);

}  // namespace nxmac::shader::internal
