// The storages an NCA section's bytes pass through before its hash tree (internal to nxmac::fs): AES-CTR decryption,
// and for an update's patch section (BKTR) the AesCtrEx decryption of its data and the IndirectStorage that lays it
// over the base game's section. Their tables are bucket trees.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "absl/status/statusor.h"
#include "nxmac/fs/crypto.h"
#include "nxmac/fs/storage.h"

namespace nxmac::fs::internal {

// AES-CTR decryption of a section: raw holds its bytes as stored, the first at nca_offset in the NCA (the counter
// counts the NCA's blocks, not the section's)
StoragePtr MakeAesCtrStorage(StoragePtr raw, const Aes128Key& key, std::uint64_t counter_upper,
                             std::uint64_t nca_offset);

// ---- bucket trees (Horizon's fssystem::BucketTree) ----
// A table: one L1 node (with L2 nodes for very large trees) of the entry sets' start offsets, then the entry sets, each
// a node of 16 KiB: a header (index, entry count, end offset), then the entries. Its own header (magic "BKTR", version,
// entry count) is kept in the NCA's fs header.
inline constexpr std::uint64_t kBucketTreeNodeSize = 0x4000;
inline constexpr std::uint64_t kBucketTreeHeaderSize = 0x10;
inline constexpr std::uint64_t kIndirectEntrySize = 0x14;
inline constexpr std::uint64_t kAesCtrExEntrySize = 0x10;

std::uint64_t BucketTreeNodeStorageSize(std::uint64_t entry_size, std::int64_t entry_count);
std::uint64_t BucketTreeEntryStorageSize(std::uint64_t entry_size, std::int64_t entry_count);

// The entry count of a tree header; InvalidArgument when it is not one
absl::StatusOr<std::int32_t> ParseBucketTreeHeader(std::span<const std::uint8_t> header);

struct BucketTreeEntries {
    std::vector<std::uint8_t> bytes;  // count entries of entry_size bytes, in order
    std::uint64_t end_offset = 0;     // where the last entry ends
};
absl::StatusOr<BucketTreeEntries> ReadBucketTree(const Storage& table, std::span<const std::uint8_t> header,
                                                 std::uint64_t entry_size);

// ---- update patches ----
struct AesCtrExEntry {
    std::uint64_t offset = 0;
    bool encrypted = true;
    std::uint32_t generation = 0;  // replaces the generation in the counter's upper half
};
absl::StatusOr<std::vector<AesCtrExEntry>> ParseAesCtrExEntries(const BucketTreeEntries& tree);

// The patch data of an update's section: raw as stored, decrypted with the generation of the entry each byte falls in;
// [0, end) of raw.
absl::StatusOr<StoragePtr> MakeAesCtrExStorage(StoragePtr raw, std::vector<AesCtrExEntry> entries, std::uint64_t end,
                                               const Aes128Key& key, std::uint64_t counter_upper,
                                               std::uint64_t nca_offset);

struct IndirectEntry {
    std::uint64_t virtual_offset = 0;
    std::uint64_t physical_offset = 0;
    std::uint32_t storage_index = 0;  // 0: the base game's section, 1: the patch data
};
absl::StatusOr<std::vector<IndirectEntry>> ParseIndirectEntries(const BucketTreeEntries& tree);

// The patched section, [0, end): each range read from the storage its entry names, at its physical offset
absl::StatusOr<StoragePtr> MakeIndirectStorage(std::array<StoragePtr, 2> storages, std::vector<IndirectEntry> entries,
                                               std::uint64_t end);

}  // namespace nxmac::fs::internal
