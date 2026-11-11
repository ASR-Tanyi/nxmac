#include "nxmac/fs/romfs.h"

#include <utility>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "bytes.h"

namespace nxmac::fs {
namespace {

using internal::LoadLe;

constexpr std::uint64_t kHeaderSize = 0x50;
constexpr std::uint32_t kNone = 0xffffffff;     // no sibling, child or entry
constexpr std::uint64_t kDirEntrySize = 0x18;   // parent, sibling, first subdirectory, first file, hash link, name size
constexpr std::uint64_t kFileEntrySize = 0x20;  // parent, sibling, data offset (8), data size (8), hash link, name size
constexpr std::uint64_t kMaxTable = 256 << 20;

// The name of the entry at offset in table, whose fixed part is fixed bytes
absl::StatusOr<std::string> Name(const std::vector<std::uint8_t>& table, std::uint64_t offset, std::uint64_t fixed,
                                 const char* what) {
    if (absl::Status s = CheckRange(offset, fixed, table.size()); !s.ok()) {
        return absl::InvalidArgumentError(absl::StrFormat("RomFS %s entry at %#x is outside its table", what, offset));
    }
    const auto size = LoadLe<std::uint32_t>(table, offset + fixed - 4);
    if (absl::Status s = CheckRange(offset + fixed, size, table.size()); !s.ok()) {
        return absl::InvalidArgumentError(
            absl::StrFormat("RomFS %s entry at %#x: its name leaves the table", what, offset));
    }
    return std::string(table.begin() + static_cast<std::ptrdiff_t>(offset + fixed),
                       table.begin() + static_cast<std::ptrdiff_t>(offset + fixed + size));
}

}  // namespace

absl::StatusOr<RomFs> RomFs::Open(StoragePtr storage) {
    if (storage->size() < kHeaderSize) return absl::InvalidArgumentError("too small for a RomFS header");
    absl::StatusOr<std::vector<std::uint8_t>> header = ReadBytes(*storage, 0, kHeaderSize);
    if (!header.ok()) return header.status();
    if (LoadLe<std::uint64_t>(*header, 0) != kHeaderSize) {
        return absl::InvalidArgumentError("not a RomFS: wrong header size");
    }
    const auto dir_offset = LoadLe<std::uint64_t>(*header, 0x18);
    const auto dir_size = LoadLe<std::uint64_t>(*header, 0x20);
    const auto file_offset = LoadLe<std::uint64_t>(*header, 0x38);
    const auto file_size = LoadLe<std::uint64_t>(*header, 0x40);
    const auto data_offset = LoadLe<std::uint64_t>(*header, 0x48);
    if (dir_size > kMaxTable || file_size > kMaxTable || dir_size < kDirEntrySize) {
        return absl::InvalidArgumentError("RomFS tables of impossible sizes");
    }
    absl::StatusOr<std::vector<std::uint8_t>> dirs = ReadBytes(*storage, dir_offset, dir_size);
    if (!dirs.ok()) return dirs.status();
    absl::StatusOr<std::vector<std::uint8_t>> files = ReadBytes(*storage, file_offset, file_size);
    if (!files.ok()) return files.status();

    RomFs fs;
    fs.storage_ = std::move(storage);
    // Every entry is visited once; more visits than the tables have room for means their links go round in a circle
    const std::uint64_t max_visits = dir_size / kDirEntrySize + file_size / kFileEntrySize;
    std::uint64_t visits = 0;
    std::vector<std::pair<std::uint32_t, std::string>> pending = {{0, ""}};
    while (!pending.empty()) {
        auto [dir, path] = std::move(pending.back());
        pending.pop_back();
        if (++visits > max_visits) return absl::InvalidArgumentError("RomFS directory links go round in a circle");
        absl::StatusOr<std::string> name = Name(*dirs, dir, kDirEntrySize, "directory");
        if (!name.ok()) return name.status();
        if (dir != 0) {
            absl::StrAppend(&path, "/", *name);
            fs.entries_.push_back({path, true, 0, 0});
        }
        for (std::uint32_t file = LoadLe<std::uint32_t>(*dirs, dir + 0xc); file != kNone;) {
            if (++visits > max_visits) return absl::InvalidArgumentError("RomFS file links go round in a circle");
            absl::StatusOr<std::string> file_name = Name(*files, file, kFileEntrySize, "file");
            if (!file_name.ok()) return file_name.status();
            Entry entry{absl::StrCat(path, "/", *file_name), false,
                        data_offset + LoadLe<std::uint64_t>(*files, file + 8),
                        LoadLe<std::uint64_t>(*files, file + 0x10)};
            if (absl::Status s = CheckRange(entry.offset, entry.size, fs.storage_->size()); !s.ok()) {
                return absl::InvalidArgumentError(
                    absl::StrFormat("RomFS file %s: its data is outside the image: %s", entry.path, s.message()));
            }
            fs.entries_.push_back(std::move(entry));
            file = LoadLe<std::uint32_t>(*files, file + 4);
        }
        std::vector<std::uint32_t> children;
        for (std::uint32_t child = LoadLe<std::uint32_t>(*dirs, dir + 8); child != kNone;) {
            if (children.size() > max_visits) {
                return absl::InvalidArgumentError("RomFS directory links go round in a circle");
            }
            if (absl::Status s = CheckRange(child, kDirEntrySize, dirs->size()); !s.ok()) {
                return absl::InvalidArgumentError(
                    absl::StrFormat("RomFS directory entry at %#x is outside its table", child));
            }
            children.push_back(child);
            child = LoadLe<std::uint32_t>(*dirs, child + 4);
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it) pending.emplace_back(*it, path);
    }
    return fs;
}

absl::StatusOr<StoragePtr> RomFs::OpenFile(const Entry& file) const {
    if (file.directory) return absl::InvalidArgumentError(absl::StrCat(file.path, " is a directory"));
    return MakeSubStorage(storage_, file.offset, file.size);
}

}  // namespace nxmac::fs
