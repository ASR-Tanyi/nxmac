// nsp's copy engine: items (a file of the ExeFS, of the RomFS, ...) read through their storages' decryption and hash
// checks on several threads, in pieces, and written where they go.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "nxmac/fs/storage.h"

namespace nxmac::nsp {

struct Destination {
    enum class Kind { kNone, kFile, kSharedFile };
    Kind kind = Kind::kNone;   // kNone: read and checked only (verify)
    std::string path;          // kFile: a file created beforehand, opened for each piece
    int fd = -1;               // kSharedFile: a file several items go into (a RomFS image), kept open by the caller
    std::uint64_t offset = 0;  // kSharedFile: where the item starts in it
};

struct Item {
    std::string name;  // for messages: "RomFS/Pack/Bootup.pack"
    fs::StoragePtr source;
    Destination destination;
};

struct Failure {
    std::size_t item;
    absl::Status status;  // DataLoss: a hash check failed
};

struct CopyResult {
    std::vector<Failure> failures;  // one per failed item, in item order
    std::uint64_t bytes = 0;        // read (and written)
    double seconds = 0;
};

using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;

// Reads every item in pieces of up to 8 MiB on threads threads and writes each piece where it goes. An item that fails
// is reported once and the rest of it skipped; the other items go on. progress, when set, is called on the calling
// thread a few times a second, and once at the end.
CopyResult CopyItems(const std::vector<Item>& items, int threads, const Progress& progress = nullptr);

// Writes all of bytes at offset (pwrite, until done)
absl::Status WriteAt(int fd, std::span<const std::uint8_t> bytes, std::uint64_t offset, const std::string& what);

}  // namespace nxmac::nsp
