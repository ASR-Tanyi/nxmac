#include "section_storage.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "absl/strings/str_format.h"
#include "bytes.h"

namespace nxmac::fs::internal {
namespace {

class AesCtrStorage final : public Storage {
public:
    AesCtrStorage(StoragePtr raw, const Aes128Key& key, std::uint64_t counter_upper, std::uint64_t nca_offset)
        : raw_(std::move(raw)), key_(key), counter_upper_(counter_upper), nca_offset_(nca_offset) {}

    absl::Status Read(std::uint64_t offset, std::span<std::uint8_t> out) const override {
        if (absl::Status s = raw_->Read(offset, out); !s.ok()) return s;
        AesCtrTransform(key_, counter_upper_, nca_offset_ + offset, out);
        return absl::OkStatus();
    }

    std::uint64_t size() const override { return raw_->size(); }

private:
    StoragePtr raw_;
    Aes128Key key_;
    std::uint64_t counter_upper_;
    std::uint64_t nca_offset_;
};

// The index of the entry that holds offset: the last whose start is not after it (entries start at 0, in order)
template <typename Entry, typename Start>
std::size_t EntryAt(const std::vector<Entry>& entries, std::uint64_t offset, Start start) {
    const auto it = std::upper_bound(entries.begin(), entries.end(), offset,
                                     [&](std::uint64_t value, const Entry& e) { return value < start(e); });
    return static_cast<std::size_t>(it - entries.begin()) - 1;
}

class AesCtrExStorage final : public Storage {
public:
    AesCtrExStorage(StoragePtr raw, std::vector<AesCtrExEntry> entries, std::uint64_t end, const Aes128Key& key,
                    std::uint64_t counter_upper, std::uint64_t nca_offset)
        : raw_(std::move(raw)),
          entries_(std::move(entries)),
          end_(end),
          key_(key),
          counter_upper_(counter_upper),
          nca_offset_(nca_offset) {}

    absl::Status Read(std::uint64_t offset, std::span<std::uint8_t> out) const override {
        if (absl::Status s = CheckRange(offset, out.size(), end_); !s.ok()) return s;
        if (absl::Status s = raw_->Read(offset, out); !s.ok()) return s;
        std::uint64_t pos = offset;
        std::size_t i = out.empty() ? 0 : EntryAt(entries_, pos, [](const AesCtrExEntry& e) { return e.offset; });
        while (pos < offset + out.size()) {
            const std::uint64_t entry_end = i + 1 < entries_.size() ? entries_[i + 1].offset : end_;
            const std::uint64_t length = std::min(entry_end, offset + out.size()) - pos;
            if (entries_[i].encrypted) {
                const std::uint64_t upper = (counter_upper_ & 0xffffffff00000000) | entries_[i].generation;
                AesCtrTransform(key_, upper, nca_offset_ + pos, out.subspan(pos - offset, length));
            }
            pos += length;
            ++i;
        }
        return absl::OkStatus();
    }

    std::uint64_t size() const override { return end_; }

private:
    StoragePtr raw_;
    std::vector<AesCtrExEntry> entries_;
    std::uint64_t end_;
    Aes128Key key_;
    std::uint64_t counter_upper_;
    std::uint64_t nca_offset_;
};

class IndirectStorage final : public Storage {
public:
    IndirectStorage(std::array<StoragePtr, 2> storages, std::vector<IndirectEntry> entries, std::uint64_t end)
        : storages_(std::move(storages)), entries_(std::move(entries)), end_(end) {}

    absl::Status Read(std::uint64_t offset, std::span<std::uint8_t> out) const override {
        if (absl::Status s = CheckRange(offset, out.size(), end_); !s.ok()) return s;
        std::uint64_t pos = offset;
        std::size_t i =
            out.empty() ? 0 : EntryAt(entries_, pos, [](const IndirectEntry& e) { return e.virtual_offset; });
        while (pos < offset + out.size()) {
            const IndirectEntry& e = entries_[i];
            const std::uint64_t entry_end = i + 1 < entries_.size() ? entries_[i + 1].virtual_offset : end_;
            const std::uint64_t length = std::min(entry_end, offset + out.size()) - pos;
            const std::uint64_t physical = e.physical_offset + (pos - e.virtual_offset);
            if (absl::Status s = storages_[e.storage_index]->Read(physical, out.subspan(pos - offset, length));
                !s.ok()) {
                return absl::Status(
                    s.code(),
                    absl::StrFormat("patched bytes %#x-%#x, from the %s at %#x: %s", pos, pos + length,
                                    e.storage_index == 0 ? "base game" : "patch data", physical, s.message()));
            }
            pos += length;
            ++i;
        }
        return absl::OkStatus();
    }

    std::uint64_t size() const override { return end_; }

private:
    std::array<StoragePtr, 2> storages_;
    std::vector<IndirectEntry> entries_;
    std::uint64_t end_;
};

constexpr std::uint64_t kNodeHeaderSize = 0x10;

std::uint64_t DivideUp(std::uint64_t a, std::uint64_t b) { return (a + b - 1) / b; }

std::uint64_t EntriesPerNode(std::uint64_t entry_size) { return (kBucketTreeNodeSize - kNodeHeaderSize) / entry_size; }
std::uint64_t OffsetsPerNode() { return (kBucketTreeNodeSize - kNodeHeaderSize) / sizeof(std::int64_t); }
std::uint64_t EntrySetCount(std::uint64_t entry_size, std::int64_t entry_count) {
    return DivideUp(static_cast<std::uint64_t>(entry_count), EntriesPerNode(entry_size));
}

// Validates that the entries' start offsets begin at 0, increase, and stay below end
template <typename Entry, typename Start>
absl::Status CheckOrder(const std::vector<Entry>& entries, std::uint64_t end, Start start, const char* what) {
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::uint64_t at = start(entries[i]);
        if ((i == 0 && at != 0) || (i > 0 && at <= start(entries[i - 1])) || at >= end) {
            return absl::InvalidArgumentError(absl::StrFormat("%s entry %u starts at %#x: out of order", what, i, at));
        }
    }
    return absl::OkStatus();
}

}  // namespace

StoragePtr MakeAesCtrStorage(StoragePtr raw, const Aes128Key& key, std::uint64_t counter_upper,
                             std::uint64_t nca_offset) {
    return std::make_shared<AesCtrStorage>(std::move(raw), key, counter_upper, nca_offset);
}

// As Horizon's BucketTree::QueryNodeStorageSize and QueryEntryStorageSize: the L1 node, the L2 nodes a tree of more
// entry sets than one node has offsets for needs, then one node per entry set
std::uint64_t BucketTreeNodeStorageSize(std::uint64_t entry_size, std::int64_t entry_count) {
    if (entry_count <= 0) return 0;
    const std::uint64_t offsets = OffsetsPerNode();
    const std::uint64_t sets = EntrySetCount(entry_size, entry_count);
    std::uint64_t l2_nodes = 0;
    if (sets > offsets) {
        const std::uint64_t l2 = DivideUp(sets, offsets);
        l2_nodes = DivideUp(sets - (offsets - (l2 - 1)), offsets);
    }
    return (1 + l2_nodes) * kBucketTreeNodeSize;
}

std::uint64_t BucketTreeEntryStorageSize(std::uint64_t entry_size, std::int64_t entry_count) {
    if (entry_count <= 0) return 0;
    return EntrySetCount(entry_size, entry_count) * kBucketTreeNodeSize;
}

absl::StatusOr<std::int32_t> ParseBucketTreeHeader(std::span<const std::uint8_t> header) {
    if (header.size() < kBucketTreeHeaderSize || std::memcmp(header.data(), "BKTR", 4) != 0) {
        return absl::InvalidArgumentError("bucket tree header without its BKTR magic");
    }
    const auto version = LoadLe<std::uint32_t>(header, 4);
    const auto count = LoadLe<std::int32_t>(header, 8);
    if (version > 1) return absl::UnimplementedError(absl::StrFormat("bucket tree version %u", version));
    if (count < 0) return absl::InvalidArgumentError(absl::StrFormat("bucket tree of %d entries", count));
    return count;
}

absl::StatusOr<BucketTreeEntries> ReadBucketTree(const Storage& table, std::span<const std::uint8_t> header,
                                                 std::uint64_t entry_size) {
    absl::StatusOr<std::int32_t> count = ParseBucketTreeHeader(header);
    if (!count.ok()) return count.status();
    BucketTreeEntries tree;
    if (*count == 0) return tree;
    const std::uint64_t nodes = BucketTreeNodeStorageSize(entry_size, *count);
    const std::uint64_t sets = EntrySetCount(entry_size, *count);
    if (absl::Status s = CheckRange(0, nodes + sets * kBucketTreeNodeSize, table.size()); !s.ok()) {
        return absl::InvalidArgumentError(
            absl::StrFormat("bucket tree of %d entries: its table is too small: %s", *count, s.message()));
    }
    absl::StatusOr<std::vector<std::uint8_t>> l1 = ReadBytes(table, 0, kNodeHeaderSize);
    if (!l1.ok()) return l1.status();
    const auto end = LoadLe<std::int64_t>(*l1, 8);
    if (end <= 0) return absl::InvalidArgumentError("bucket tree ends at offset 0");
    tree.end_offset = static_cast<std::uint64_t>(end);
    tree.bytes.reserve(static_cast<std::size_t>(*count) * entry_size);
    for (std::uint64_t set = 0; set < sets; ++set) {
        absl::StatusOr<std::vector<std::uint8_t>> node =
            ReadBytes(table, nodes + set * kBucketTreeNodeSize, kBucketTreeNodeSize);
        if (!node.ok()) return node.status();
        const auto index = LoadLe<std::int32_t>(*node, 0);
        const auto in_set = LoadLe<std::int32_t>(*node, 4);
        if (index != static_cast<std::int32_t>(set) || in_set <= 0 ||
            static_cast<std::uint64_t>(in_set) > EntriesPerNode(entry_size)) {
            return absl::InvalidArgumentError(
                absl::StrFormat("bucket tree entry set %u: header says index %d, %d entries", set, index, in_set));
        }
        tree.bytes.insert(tree.bytes.end(), node->begin() + kNodeHeaderSize,
                          node->begin() + static_cast<std::ptrdiff_t>(kNodeHeaderSize + in_set * entry_size));
    }
    if (tree.bytes.size() != static_cast<std::size_t>(*count) * entry_size) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "bucket tree: its entry sets hold %u entries, its header says %d", tree.bytes.size() / entry_size, *count));
    }
    return tree;
}

absl::StatusOr<std::vector<AesCtrExEntry>> ParseAesCtrExEntries(const BucketTreeEntries& tree) {
    std::vector<AesCtrExEntry> entries(tree.bytes.size() / kAesCtrExEntrySize);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::span<const std::uint8_t> e(tree.bytes.data() + i * kAesCtrExEntrySize, kAesCtrExEntrySize);
        entries[i].offset = LoadLe<std::uint64_t>(e, 0);
        if (e[8] > 1) return absl::InvalidArgumentError(absl::StrFormat("AesCtrEx entry %u: encryption %u", i, e[8]));
        entries[i].encrypted = e[8] == 0;
        entries[i].generation = LoadLe<std::uint32_t>(e, 0xc);
    }
    if (absl::Status s = CheckOrder(
            entries, tree.end_offset, [](const AesCtrExEntry& e) { return e.offset; }, "AesCtrEx");
        !s.ok()) {
        return s;
    }
    return entries;
}

absl::StatusOr<std::vector<IndirectEntry>> ParseIndirectEntries(const BucketTreeEntries& tree) {
    std::vector<IndirectEntry> entries(tree.bytes.size() / kIndirectEntrySize);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::span<const std::uint8_t> e(tree.bytes.data() + i * kIndirectEntrySize, kIndirectEntrySize);
        entries[i].virtual_offset = LoadLe<std::uint64_t>(e, 0);
        entries[i].physical_offset = LoadLe<std::uint64_t>(e, 8);
        entries[i].storage_index = LoadLe<std::uint32_t>(e, 0x10);
        if (entries[i].storage_index > 1) {
            return absl::InvalidArgumentError(
                absl::StrFormat("indirect entry %u: storage %u (0 and 1 exist)", i, entries[i].storage_index));
        }
    }
    if (absl::Status s = CheckOrder(
            entries, tree.end_offset, [](const IndirectEntry& e) { return e.virtual_offset; }, "indirect");
        !s.ok()) {
        return s;
    }
    return entries;
}

absl::StatusOr<StoragePtr> MakeAesCtrExStorage(StoragePtr raw, std::vector<AesCtrExEntry> entries, std::uint64_t end,
                                               const Aes128Key& key, std::uint64_t counter_upper,
                                               std::uint64_t nca_offset) {
    if (entries.empty() && end > 0) return absl::InvalidArgumentError("AesCtrEx storage without entries");
    if (absl::Status s = CheckRange(0, end, raw->size()); !s.ok()) {
        return absl::InvalidArgumentError(absl::StrFormat("AesCtrEx entries end past the patch data: %s", s.message()));
    }
    return std::make_shared<AesCtrExStorage>(std::move(raw), std::move(entries), end, key, counter_upper, nca_offset);
}

absl::StatusOr<StoragePtr> MakeIndirectStorage(std::array<StoragePtr, 2> storages, std::vector<IndirectEntry> entries,
                                               std::uint64_t end) {
    if (entries.empty() && end > 0) return absl::InvalidArgumentError("indirect storage without entries");
    return std::make_shared<IndirectStorage>(std::move(storages), std::move(entries), end);
}

}  // namespace nxmac::fs::internal
