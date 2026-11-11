#include "integrity.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"
#include "bytes.h"
#include "nxmac/fs/crypto.h"

namespace nxmac::fs::internal {
namespace {

constexpr std::uint64_t kHashSize = 32;

class HashCheckedStorage final : public Storage {
public:
    HashCheckedStorage(StoragePtr data, StoragePtr hashes, std::uint64_t block_size, bool pad_last_block,
                       std::string what)
        : data_(std::move(data)),
          hashes_(std::move(hashes)),
          block_size_(block_size),
          pad_last_block_(pad_last_block),
          what_(std::move(what)) {}

    absl::Status Read(std::uint64_t offset, std::span<std::uint8_t> out) const override {
        if (absl::Status s = CheckRange(offset, out.size(), size()); !s.ok()) return s;
        if (out.empty()) return absl::OkStatus();
        const std::uint64_t first = offset / block_size_;
        const std::uint64_t last = (offset + out.size() - 1) / block_size_;
        const std::uint64_t blocks = last - first + 1;
        absl::StatusOr<std::vector<std::uint8_t>> hashes = ReadBytes(*hashes_, first * kHashSize, blocks * kHashSize);
        if (!hashes.ok()) return hashes.status();
        // Whole blocks, zeros past the end of the data
        const std::uint64_t begin = first * block_size_;
        const std::uint64_t end = std::min((last + 1) * block_size_, size());
        std::vector<std::uint8_t> buffer(blocks * block_size_);
        if (absl::Status s = data_->Read(begin, std::span(buffer.data(), end - begin)); !s.ok()) return s;
        for (std::uint64_t i = 0; i < blocks; ++i) {
            const std::uint64_t block_begin = begin + i * block_size_;
            const std::uint64_t length = pad_last_block_ ? block_size_ : std::min(block_size_, size() - block_begin);
            const Sha256Hash hash = Sha256(std::span(buffer.data() + i * block_size_, length));
            if (std::memcmp(hash.data(), hashes->data() + i * kHashSize, kHashSize) != 0) {
                return absl::DataLossError(absl::StrFormat("%s: block %u (bytes %#x-%#x) fails its hash check", what_,
                                                           first + i, block_begin,
                                                           std::min(block_begin + block_size_, size())));
            }
        }
        std::memcpy(out.data(), buffer.data() + (offset - begin), out.size());
        return absl::OkStatus();
    }

    std::uint64_t size() const override { return data_->size(); }

private:
    StoragePtr data_;
    StoragePtr hashes_;
    std::uint64_t block_size_;
    bool pad_last_block_;
    std::string what_;
};

}  // namespace

absl::StatusOr<StoragePtr> MakeHashCheckedStorage(StoragePtr data, StoragePtr hashes, std::uint64_t block_size,
                                                  bool pad_last_block, std::string what) {
    if (block_size == 0 || (block_size & (block_size - 1)) != 0) {
        return absl::InvalidArgumentError(
            absl::StrFormat("%s: hash block size %#x is not a power of two", what, block_size));
    }
    const std::uint64_t blocks = (data->size() + block_size - 1) / block_size;
    if (hashes->size() / kHashSize < blocks) {
        return absl::InvalidArgumentError(
            absl::StrFormat("%s: %#x bytes of hashes for %u blocks", what, hashes->size(), blocks));
    }
    return std::make_shared<HashCheckedStorage>(std::move(data), std::move(hashes), block_size, pad_last_block,
                                                std::move(what));
}

absl::StatusOr<StoragePtr> OpenHierarchicalSha256(const StoragePtr& raw, std::span<const std::uint8_t> hash_data) {
    // master hash (0x20), block size, layer count, then (offset, size) of each layer: the hash table, then the data
    if (hash_data.size() < 0x48) return absl::InvalidArgumentError("HierarchicalSha256 info is too small");
    const auto block_size = LoadLe<std::uint32_t>(hash_data, 0x20);
    const auto layers = LoadLe<std::uint32_t>(hash_data, 0x24);
    if (layers != 2) {
        return absl::UnimplementedError(
            absl::StrFormat("HierarchicalSha256 with %u layers (only 2 are known)", layers));
    }
    const auto table_offset = LoadLe<std::uint64_t>(hash_data, 0x28);
    const auto table_size = LoadLe<std::uint64_t>(hash_data, 0x30);
    const auto data_offset = LoadLe<std::uint64_t>(hash_data, 0x38);
    const auto data_size = LoadLe<std::uint64_t>(hash_data, 0x40);
    constexpr std::uint64_t kMaxTable = 64 << 20;
    if (table_size > kMaxTable) return absl::InvalidArgumentError("HierarchicalSha256 hash table is too large");
    if (absl::Status s = CheckRange(table_offset, table_size, raw->size()); !s.ok()) return s;
    absl::StatusOr<std::vector<std::uint8_t>> table = ReadBytes(*raw, table_offset, table_size);
    if (!table.ok()) return table.status();
    if (std::memcmp(Sha256(*table).data(), hash_data.data(), kHashSize) != 0) {
        return absl::DataLossError(
            "the section's hash table fails its master hash (a wrong key decrypts to other bytes; or the NCA is "
            "damaged)");
    }
    absl::StatusOr<StoragePtr> data = MakeSubStorage(raw, data_offset, data_size);
    if (!data.ok()) return data.status();
    return MakeHashCheckedStorage(*std::move(data), MakeMemoryStorage(*std::move(table)), block_size, false,
                                  "HierarchicalSha256 data");
}

absl::StatusOr<StoragePtr> OpenHierarchicalIntegrity(const StoragePtr& raw, std::span<const std::uint8_t> hash_data) {
    // "IVFC", version, master hash size, layer count (with the master hash), 6 levels of (offset, size, log2 block
    // size, reserved), a salt (0x20), then the master hash at 0xc0
    constexpr std::size_t kLevels = 0x10;
    constexpr std::size_t kLevelSize = 0x18;
    constexpr std::size_t kMasterHash = 0xc0;
    if (hash_data.size() < kMasterHash + kHashSize) return absl::InvalidArgumentError("IVFC info is too small");
    if (std::memcmp(hash_data.data(), "IVFC", 4) != 0) return absl::InvalidArgumentError("IVFC info without its magic");
    const auto master_size = LoadLe<std::uint32_t>(hash_data, 0x8);
    const auto layers = LoadLe<std::uint32_t>(hash_data, 0xc);
    if (master_size == 0 || master_size % kHashSize != 0 || kMasterHash + master_size > hash_data.size()) {
        return absl::InvalidArgumentError(absl::StrFormat("IVFC master hash of %#x bytes", master_size));
    }
    if (layers < 2 || layers > 7) return absl::InvalidArgumentError(absl::StrFormat("IVFC with %u layers", layers));
    StoragePtr hashes = MakeMemoryStorage(
        std::vector<std::uint8_t>(hash_data.begin() + kMasterHash, hash_data.begin() + kMasterHash + master_size));
    for (std::uint32_t level = 0; level + 1 < layers; ++level) {
        const std::size_t at = kLevels + level * kLevelSize;
        const auto offset = LoadLe<std::uint64_t>(hash_data, at);
        const auto size = LoadLe<std::uint64_t>(hash_data, at + 8);
        const auto order = LoadLe<std::uint32_t>(hash_data, at + 0x10);
        if (order < 4 || order > 24) {
            return absl::InvalidArgumentError(absl::StrFormat("IVFC level %u: block size 2^%u", level, order));
        }
        absl::StatusOr<StoragePtr> data = MakeSubStorage(raw, offset, size);
        if (!data.ok()) return data.status();
        absl::StatusOr<StoragePtr> checked =
            MakeHashCheckedStorage(*std::move(data), std::move(hashes), std::uint64_t{1} << order, true,
                                   level + 2 == layers ? "IVFC data" : absl::StrFormat("IVFC level %u", level));
        if (!checked.ok()) return checked.status();
        hashes = *std::move(checked);
        // A hash level is read and checked once, here, and kept: without it every read of the data, however small,
        // would check a block of each level above it again. The levels of a 16 GB RomFS take 32 MB. A level that fails
        // its check stays as it was, so that the reads it fails name the files they are for.
        constexpr std::uint64_t kMaxKept = 256 << 20;
        if (level + 2 < layers && size <= kMaxKept) {
            if (absl::StatusOr<std::vector<std::uint8_t>> level_bytes = ReadBytes(*hashes, 0, size); level_bytes.ok()) {
                hashes = MakeMemoryStorage(*std::move(level_bytes));
            }
        }
    }
    return hashes;
}

}  // namespace nxmac::fs::internal
