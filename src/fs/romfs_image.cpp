#include "nxmac/fs/romfs_image.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "bytes.h"

namespace nxmac::fs {
namespace {

using internal::LoadLe;
using internal::StoreLe;

constexpr std::size_t kMaxName = 0x300;

// The host's rule for names below the root
bool ValidName(std::string_view name) {
    return !name.empty() && name.size() <= kMaxName && !absl::StrContains(name, '\0') &&
           !absl::StrContains(name, '/') && name != "." && name != "..";
}

// Ordered by their bytes, a prefix before the longer name (C#'s SequenceCompareTo, the host's memcmp then length)
bool NameLess(std::string_view a, std::string_view b) {
    const int c = std::memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
    return c < 0 || (c == 0 && a.size() < b.size());
}

}  // namespace

absl::StatusOr<RomFsImageLayout> RomFsImageLayout::Plan(const std::vector<RomFs::Entry>& in) {
    struct Node {
        RomFsImageEntry entry;
        std::vector<std::size_t> children;
    };
    std::vector<Node> nodes(1);
    nodes[0].entry.directory = true;
    absl::flat_hash_map<std::string, std::size_t> by_path = {{"", 0}};
    for (std::size_t i = 0; i < in.size(); ++i) {
        const RomFs::Entry& e = in[i];
        const std::size_t slash = e.path.rfind('/');
        if (slash == std::string::npos) {
            return absl::InvalidArgumentError(absl::StrCat("RomFS path without a /: ", e.path));
        }
        Node node;
        node.entry.name = e.path.substr(slash + 1);
        node.entry.path = e.path;
        node.entry.directory = e.directory;
        node.entry.b = e.directory ? 0 : e.size;
        node.entry.source = i;
        if (!ValidName(node.entry.name)) {
            return absl::InvalidArgumentError(absl::StrCat("RomFS name the image cannot hold: ", e.path));
        }
        const auto found = by_path.find(e.path.substr(0, slash));
        if (found == by_path.end() || !nodes[found->second].entry.directory) {
            return absl::InvalidArgumentError(absl::StrCat("RomFS entry listed before its directory: ", e.path));
        }
        const std::size_t parent = found->second;  // the iterator does not outlive the emplace below
        if (!by_path.emplace(e.path, nodes.size()).second) {
            return absl::InvalidArgumentError(absl::StrCat("RomFS path listed twice: ", e.path));
        }
        nodes[parent].children.push_back(nodes.size());
        nodes.push_back(std::move(node));
    }

    // Breadth first: each directory's children, sorted, follow the entries before them
    RomFsImageLayout layout;
    std::vector<std::size_t> order = {0};
    for (std::size_t k = 0; k < order.size(); ++k) {
        Node& node = nodes[order[k]];
        if (!node.entry.directory) continue;
        std::sort(node.children.begin(), node.children.end(),
                  [&](std::size_t a, std::size_t b) { return NameLess(nodes[a].entry.name, nodes[b].entry.name); });
        node.entry.a = order.size();
        node.entry.b = node.children.size();
        order.insert(order.end(), node.children.begin(), node.children.end());
    }
    layout.entries_.reserve(order.size());
    std::uint64_t offset = kHeaderSize;
    for (const std::size_t n : order) {
        RomFsImageEntry& e = nodes[n].entry;
        if (!e.directory) {
            e.a = offset;
            offset += e.b;
        }
        layout.names_size_ += e.name.size();
        layout.entries_.push_back(std::move(e));
    }
    layout.index_offset_ = offset;
    if (layout.names_size_ > std::numeric_limits<std::uint32_t>::max()) {
        return absl::InvalidArgumentError("RomFS names too long for the image's 32-bit name offsets");
    }
    return layout;
}

std::vector<std::uint8_t> RomFsImageLayout::Index() const {
    std::vector<std::uint8_t> index(entries_.size() * kEntrySize + names_size_);
    std::uint64_t name = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const RomFsImageEntry& e = entries_[i];
        const std::size_t at = i * kEntrySize;
        StoreLe<std::uint64_t>(index, at, e.a);
        StoreLe<std::uint64_t>(index, at + 8, e.b);
        StoreLe<std::uint32_t>(index, at + 0x10, static_cast<std::uint32_t>(name));
        StoreLe<std::uint16_t>(index, at + 0x14, static_cast<std::uint16_t>(e.name.size()));
        index[at + 0x16] = e.directory ? 0 : 1;
        index[at + 0x17] = e.damaged ? 1 : 0;
        std::copy(e.name.begin(), e.name.end(),
                  index.begin() + static_cast<std::ptrdiff_t>(entries_.size() * kEntrySize + name));
        name += e.name.size();
    }
    return index;
}

std::array<std::uint8_t, RomFsImageLayout::kHeaderSize> RomFsImageLayout::Header() const {
    std::array<std::uint8_t, kHeaderSize> header{};
    std::memcpy(header.data(), "DMROMFS1", 8);
    StoreLe<std::uint64_t>(header, 8, index_offset_);
    StoreLe<std::uint64_t>(header, 16, entries_.size() * kEntrySize + names_size_);
    StoreLe<std::uint64_t>(header, 24, entries_.size());
    return header;
}

std::uint64_t RomFsImageLayout::size() const { return index_offset_ + entries_.size() * kEntrySize + names_size_; }

absl::StatusOr<RomFsImage> RomFsImage::Open(StoragePtr storage) {
    constexpr std::uint64_t kHeader = RomFsImageLayout::kHeaderSize;
    constexpr std::uint64_t kEntry = RomFsImageLayout::kEntrySize;
    const auto invalid = [](const std::string& why) {
        return absl::InvalidArgumentError("not a valid RomFS image: " + why);
    };
    if (storage->size() < kHeader) return invalid("shorter than its header");
    absl::StatusOr<std::vector<std::uint8_t>> header = ReadBytes(*storage, 0, kHeader);
    if (!header.ok()) return header.status();
    if (std::memcmp(header->data(), "DMROMFS1", 8) != 0) return invalid("no DMROMFS1 magic");
    const auto index_offset = LoadLe<std::uint64_t>(*header, 8);
    const auto index_size = LoadLe<std::uint64_t>(*header, 16);
    const auto count = LoadLe<std::uint64_t>(*header, 24);
    if (count == 0 || count > std::numeric_limits<std::uint32_t>::max() || index_offset < kHeader ||
        index_offset > storage->size() || index_size != storage->size() - index_offset || count > index_size / kEntry) {
        return invalid("its header does not describe its index");
    }
    constexpr std::uint64_t kMaxIndex = std::uint64_t{1} << 30;
    if (index_size > kMaxIndex) return invalid("an index of more than 1 GiB");
    absl::StatusOr<std::vector<std::uint8_t>> index = ReadBytes(*storage, index_offset, index_size);
    if (!index.ok()) return index.status();
    const std::uint64_t pool = index_size - count * kEntry;
    const auto* names = reinterpret_cast<const char*>(index->data() + count * kEntry);

    RomFsImage image;
    image.storage_ = std::move(storage);
    image.entries_.resize(count);
    std::uint64_t next_child = 1;
    for (std::uint64_t k = 0; k < count; ++k) {
        const std::size_t at = k * kEntry;
        RomFsImageEntry& e = image.entries_[k];
        e.a = LoadLe<std::uint64_t>(*index, at);
        e.b = LoadLe<std::uint64_t>(*index, at + 8);
        const auto name = LoadLe<std::uint32_t>(*index, at + 0x10);
        const auto length = LoadLe<std::uint16_t>(*index, at + 0x14);
        const std::uint8_t type = (*index)[at + 0x16];
        const std::uint8_t flags = (*index)[at + 0x17];
        if (name + std::uint64_t{length} > pool || type > 1 || flags > (type != 0 ? 1 : 0)) {
            return invalid(absl::StrFormat("entry %u", k));
        }
        e.name.assign(names + name, length);
        e.directory = type == 0;
        e.damaged = flags != 0;
        if (k == 0 ? (!e.directory || length != 0) : !ValidName(e.name)) {
            return invalid(absl::StrFormat("entry %u's name", k));
        }
        if (!e.directory) {
            if (e.a < kHeader || e.a > index_offset || e.b > index_offset - e.a) {
                return invalid(absl::StrFormat("entry %u's data", k));
            }
            continue;
        }
        if (e.a != next_child || e.a <= k || e.a > count || e.b > count - e.a) {
            return invalid(absl::StrFormat("entry %u's children", k));
        }
        next_child += e.b;
    }
    if (next_child != count) return invalid("entries no directory holds");
    for (const RomFsImageEntry& d : image.entries_) {
        if (!d.directory) continue;
        for (std::uint64_t c = d.a; c < d.a + d.b; ++c) {
            if (c > d.a && !NameLess(image.entries_[c - 1].name, image.entries_[c].name)) {
                return invalid(absl::StrFormat("children of %s out of order", d.path.empty() ? "/" : d.path));
            }
            image.entries_[c].path = absl::StrCat(d.path, "/", image.entries_[c].name);
        }
    }
    return image;
}

absl::StatusOr<StoragePtr> RomFsImage::OpenFile(const RomFsImageEntry& file) const {
    if (file.directory) return absl::InvalidArgumentError(absl::StrCat(file.path, " is a directory"));
    return MakeSubStorage(storage_, file.a, file.b);
}

}  // namespace nxmac::fs
