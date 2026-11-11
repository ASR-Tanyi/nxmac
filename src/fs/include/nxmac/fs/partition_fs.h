// Partition filesystems: PFS0 (an NSP, an NCA's ExeFS and other PartitionFs sections) and HFS0 (the partitions of a
// cartridge dump, XCI): a header, a table of entries, a table of names, then the files' data, one after another.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "nxmac/fs/storage.h"

namespace nxmac::fs {

class PartitionFs {
public:
    struct Entry {
        std::string name;
        std::uint64_t offset = 0;  // of the file's data, in the partition's storage
        std::uint64_t size = 0;
    };

    // InvalidArgument when storage does not hold a PFS0 or HFS0, or its tables do not fit; a last name without its NUL
    // (as some NSZ writers left it) is read up to the end of the name table.
    static absl::StatusOr<PartitionFs> Open(StoragePtr storage);

    const std::vector<Entry>& entries() const { return entries_; }
    // nullptr when there is no file of that name
    const Entry* Find(std::string_view name) const;
    absl::StatusOr<StoragePtr> OpenFile(const Entry& entry) const;

private:
    StoragePtr storage_;
    std::vector<Entry> entries_;
};

}  // namespace nxmac::fs
