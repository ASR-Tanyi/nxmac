// DMROMFS1: nxnative's RomFS image, a game's whole (patched) RomFS in one file that the host reads with pread.
//   header (32 bytes): "DMROMFS1", the index's offset, the index's size, the entry count (little-endian u64s)
//   the files' data, back to back from offset 32, in index order
//   the index: one 24-byte entry per directory and file, breadth first from the root, each directory's children
//     sorted by their names' bytes (a directory: index of its first child, child count; a file: offset of its data,
//     size; then the name's offset and length in the name pool, the kind (0 directory, 1 file) and flags (1: damaged)),
//     then the name pool
// Byte for byte what nxnative's nx_extract --romfs-image writes, and what its host's romfs_image_open accepts.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "nxmac/fs/romfs.h"
#include "nxmac/fs/storage.h"

namespace nxmac::fs {

struct RomFsImageEntry {
    std::string name;  // the last path component; "" for the root
    std::string path;  // "/dir/name"; "" for the root
    bool directory = false;
    std::uint64_t a = 0;  // a directory's first child (an index); a file's data offset in the image
    std::uint64_t b = 0;  // a directory's child count; a file's size
    bool damaged = false;
    std::size_t source = 0;  // a file's index in the entries it was planned from
};

class RomFsImageLayout {
public:
    static constexpr std::uint64_t kHeaderSize = 32;
    static constexpr std::uint64_t kEntrySize = 24;

    // The image of a RomFS's directories and files (RomFs::entries()); InvalidArgument for a name the host would refuse
    // (empty, longer than 0x300 bytes, with a NUL, ".", "..") or a path listed twice
    static absl::StatusOr<RomFsImageLayout> Plan(const std::vector<RomFs::Entry>& entries);

    const std::vector<RomFsImageEntry>& entries() const { return entries_; }
    std::vector<RomFsImageEntry>& entries() { return entries_; }  // to mark damaged files
    // Where the files' data ends and the index starts
    std::uint64_t index_offset() const { return index_offset_; }
    // The index and name pool, written at index_offset()
    std::vector<std::uint8_t> Index() const;
    // The header, written at 0 once the rest is in place
    std::array<std::uint8_t, kHeaderSize> Header() const;
    // The whole image's size
    std::uint64_t size() const;

private:
    std::vector<RomFsImageEntry> entries_;
    std::uint64_t index_offset_ = kHeaderSize;
    std::uint64_t names_size_ = 0;
};

// A DMROMFS1 image read back, validated as nxnative's host does (romfs_image_open)
class RomFsImage {
public:
    static absl::StatusOr<RomFsImage> Open(StoragePtr storage);

    // In index order: the root first, with its path ""; paths are "/dir/name"
    const std::vector<RomFsImageEntry>& entries() const { return entries_; }
    absl::StatusOr<StoragePtr> OpenFile(const RomFsImageEntry& file) const;

private:
    StoragePtr storage_;
    std::vector<RomFsImageEntry> entries_;
};

}  // namespace nxmac::fs
