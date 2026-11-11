// The hash trees of NCA sections (internal to nxmac::fs): every read is checked, block by block, against SHA-256 hashes
// that are themselves checked up to the master hash in the NCA's header, so a wrong key or a damaged byte is a DataLoss
// error rather than wrong data.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "absl/status/statusor.h"
#include "nxmac/fs/storage.h"

namespace nxmac::fs::internal {

// HierarchicalSha256 (PartitionFs sections): hash_data is the fs header's 0xf8 bytes of hash data. The data layer of
// raw, checked against the hash table, which the master hash checks when this opens.
absl::StatusOr<StoragePtr> OpenHierarchicalSha256(const StoragePtr& raw, std::span<const std::uint8_t> hash_data);

// HierarchicalIntegrity, "IVFC" (RomFS sections): the last level of raw, each level checked against the one above it,
// the first against the master hash.
absl::StatusOr<StoragePtr> OpenHierarchicalIntegrity(const StoragePtr& raw, std::span<const std::uint8_t> hash_data);

// data checked against hashes: block i of data has the SHA-256 hashes[32 * i, 32 * i + 32). The last block is hashed as
// it is, or, with pad_last_block, filled with zeros to a whole block first (IVFC). what names the layer in errors.
// InvalidArgument when hashes is too small for data's blocks, or block_size is not a power of two.
absl::StatusOr<StoragePtr> MakeHashCheckedStorage(StoragePtr data, StoragePtr hashes, std::uint64_t block_size,
                                                  bool pad_last_block, std::string what);

}  // namespace nxmac::fs::internal
