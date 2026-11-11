// Storage: a read-only range of bytes, the unit the Switch's container formats stack on each other (a file, a window of
// it, its decrypted view, its hash-checked view), as Horizon's fs::IStorage. Read is const and safe to call from
// several threads at once on every storage nxmac::fs makes.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace nxmac::fs {

class Storage {
public:
    Storage() = default;
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    virtual ~Storage() = default;

    // Fills out with the bytes at offset. A range that does not lie within the storage is OutOfRange; a block that
    // fails its hash check is DataLoss.
    virtual absl::Status Read(std::uint64_t offset, std::span<std::uint8_t> out) const = 0;
    virtual std::uint64_t size() const = 0;
};

using StoragePtr = std::shared_ptr<const Storage>;

// A file on disk, read with pread.
absl::StatusOr<StoragePtr> OpenFileStorage(const std::string& path);

// Bytes in memory.
StoragePtr MakeMemoryStorage(std::vector<std::uint8_t> bytes);

// The window [offset, offset + size) of base; OutOfRange when it does not lie within base.
absl::StatusOr<StoragePtr> MakeSubStorage(StoragePtr base, std::uint64_t offset, std::uint64_t size);

// The bytes [offset, offset + size) of storage, in a new vector.
absl::StatusOr<std::vector<std::uint8_t>> ReadBytes(const Storage& storage, std::uint64_t offset, std::uint64_t size);

// OK when [offset, offset + size) lies within [0, total), without overflowing; OutOfRange otherwise.
absl::Status CheckRange(std::uint64_t offset, std::uint64_t size, std::uint64_t total);

}  // namespace nxmac::fs
