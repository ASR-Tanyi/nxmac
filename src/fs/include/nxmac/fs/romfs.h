// RomFS: a game's read-only file tree (an NCA's data section): a header, tables of directory and file entries (with
// hash tables for lookups by name, which listing does not need), then the files' data.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "nxmac/fs/storage.h"

namespace nxmac::fs {

class RomFs {
public:
    struct Entry {
        std::string path;  // from the root, "/" separated, no trailing "/": "/Pack/Bootup.pack"; the root is ""
        bool directory = false;
        std::uint64_t offset = 0;  // a file's data, in the RomFS image
        std::uint64_t size = 0;
    };

    // Reads the entry tables; InvalidArgument when they are not a RomFS's or their links leave them.
    static absl::StatusOr<RomFs> Open(StoragePtr storage);

    // Every directory and file but the root, depth first: each directory's files, then its subdirectories, in table
    // order
    const std::vector<Entry>& entries() const { return entries_; }
    absl::StatusOr<StoragePtr> OpenFile(const Entry& file) const;

private:
    StoragePtr storage_;
    std::vector<Entry> entries_;
};

}  // namespace nxmac::fs
