#include "nxmac/fs/partition_fs.h"

#include <algorithm>
#include <utility>

#include "absl/strings/str_format.h"
#include "bytes.h"

namespace nxmac::fs {

absl::StatusOr<PartitionFs> PartitionFs::Open(StoragePtr storage) {
    using internal::LoadLe;
    constexpr std::uint64_t kHeaderSize = 0x10;
    if (storage->size() < kHeaderSize) return absl::InvalidArgumentError("too small for a partition filesystem header");
    absl::StatusOr<std::vector<std::uint8_t>> header = ReadBytes(*storage, 0, kHeaderSize);
    if (!header.ok()) return header.status();
    const std::string_view magic(reinterpret_cast<const char*>(header->data()), 4);
    std::uint64_t entry_size = 0;
    if (magic == "PFS0") {
        entry_size = 0x18;
    } else if (magic == "HFS0") {
        entry_size = 0x40;  // with the hash of the file's first bytes
    } else {
        return absl::InvalidArgumentError("not a partition filesystem (no PFS0 or HFS0 magic)");
    }
    const auto count = LoadLe<std::uint32_t>(*header, 4);
    const auto names_size = LoadLe<std::uint32_t>(*header, 8);
    const std::uint64_t tables_size = kHeaderSize + count * entry_size + names_size;
    if (tables_size > storage->size()) {
        return absl::InvalidArgumentError(
            absl::StrFormat("%s tables of %u entries do not fit in %#x bytes", magic, count, storage->size()));
    }
    absl::StatusOr<std::vector<std::uint8_t>> tables = ReadBytes(*storage, 0, tables_size);
    if (!tables.ok()) return tables.status();
    const std::span<const std::uint8_t> names(tables->data() + kHeaderSize + count * entry_size, names_size);

    PartitionFs fs;
    fs.storage_ = std::move(storage);
    fs.entries_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = kHeaderSize + i * entry_size;
        Entry entry;
        entry.offset = LoadLe<std::uint64_t>(*tables, at);
        entry.size = LoadLe<std::uint64_t>(*tables, at + 8);
        const auto name_offset = LoadLe<std::uint32_t>(*tables, at + 0x10);
        if (name_offset >= names.size()) {
            return absl::InvalidArgumentError(
                absl::StrFormat("%s entry %u: its name is outside the name table", magic, i));
        }
        const auto name_begin = names.begin() + name_offset;
        entry.name.assign(name_begin, std::find(name_begin, names.end(), std::uint8_t{0}));
        if (entry.offset > fs.storage_->size() - tables_size) {
            return absl::InvalidArgumentError(absl::StrFormat("%s entry %s starts past the end", magic, entry.name));
        }
        entry.offset += tables_size;
        if (absl::Status s = CheckRange(entry.offset, entry.size, fs.storage_->size()); !s.ok()) {
            return absl::InvalidArgumentError(absl::StrFormat("%s entry %s: %s", magic, entry.name, s.message()));
        }
        fs.entries_.push_back(std::move(entry));
    }
    return fs;
}

const PartitionFs::Entry* PartitionFs::Find(std::string_view name) const {
    for (const Entry& e : entries_) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

absl::StatusOr<StoragePtr> PartitionFs::OpenFile(const Entry& entry) const {
    return MakeSubStorage(storage_, entry.offset, entry.size);
}

}  // namespace nxmac::fs
