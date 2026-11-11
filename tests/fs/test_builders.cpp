#include "test_builders.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string_view>
#include <utility>

#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "gtest/gtest.h"

namespace nxmac::fs::testing {
namespace {

template <typename T>
void PutLe(Bytes& bytes, std::size_t offset, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

void Append(Bytes& to, const Bytes& from) { to.insert(to.end(), from.begin(), from.end()); }

void PadTo(Bytes& bytes, std::uint64_t size) { bytes.resize(std::max<std::uint64_t>(bytes.size(), size)); }

// The SHA-256 of each block_size block of data, the last one zero-padded (pad) or as it is
Bytes BlockHashes(const Bytes& data, std::uint64_t block_size, bool pad) {
    Bytes hashes;
    for (std::uint64_t at = 0; at < data.size(); at += block_size) {
        Bytes block(data.begin() + static_cast<std::ptrdiff_t>(at),
                    data.begin() + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(at + block_size, data.size())));
        if (pad) block.resize(block_size);
        const Sha256Hash hash = Sha256(block);
        hashes.insert(hashes.end(), hash.begin(), hash.end());
    }
    return hashes;
}

}  // namespace

std::string ToHex(std::span<const std::uint8_t> bytes) {
    return absl::BytesToHexString(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

Bytes PatternBytes(std::size_t size, std::uint32_t seed) {
    Bytes bytes(size);
    std::uint32_t x = seed * 2654435761u + 1;
    for (std::uint8_t& b : bytes) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<std::uint8_t>(x >> 24);
    }
    return bytes;
}

std::string TestKeysText() {
    std::string text = "; made-up keys for nxmac's tests\n# another comment\n";
    absl::StrAppend(&text, "header_key = ", ToHex(PatternBytes(32, 1)), "\n");
    absl::StrAppend(&text, "master_key_source = ", ToHex(PatternBytes(16, 2)), "  ; skipped\n");
    absl::StrAppend(&text, "key_area_key_application_source = ", ToHex(PatternBytes(16, 3)), "\n");
    for (int rev = 0; rev < 4; ++rev) {
        absl::StrAppendFormat(&text, "key_area_key_application_%02x = %s\n", rev, ToHex(PatternBytes(16, 100 + rev)));
        absl::StrAppendFormat(&text, "TITLEKEK_%02X = %s\n", rev, ToHex(PatternBytes(16, 200 + rev)));
    }
    absl::StrAppend(&text, "key_area_key_ocean_00 = ", ToHex(PatternBytes(16, 300)), "\n");
    absl::StrAppend(&text, "key_area_key_system_00 = ", ToHex(PatternBytes(16, 301)), "\n\n");
    return text;
}

KeySet TestKeys() {
    absl::StatusOr<KeySet> keys = KeySet::Parse(TestKeysText());
    EXPECT_TRUE(keys.ok()) << keys.status();
    return keys.ok() ? *keys : KeySet();
}

Bytes BuildPfs0(const std::vector<File>& files) {
    Bytes names;
    for (const File& f : files) {
        names.insert(names.end(), f.path.begin(), f.path.end());
        names.push_back(0);
    }
    names.resize(AlignUp(names.size() + 0x10 + files.size() * 0x18, 0x20) - 0x10 - files.size() * 0x18);
    Bytes out(0x10 + files.size() * 0x18);
    std::memcpy(out.data(), "PFS0", 4);
    PutLe<std::uint32_t>(out, 4, static_cast<std::uint32_t>(files.size()));
    PutLe<std::uint32_t>(out, 8, static_cast<std::uint32_t>(names.size()));
    std::uint64_t data_offset = 0;
    std::uint32_t name_offset = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        PutLe<std::uint64_t>(out, 0x10 + i * 0x18, data_offset);
        PutLe<std::uint64_t>(out, 0x18 + i * 0x18, files[i].data.size());
        PutLe<std::uint32_t>(out, 0x20 + i * 0x18, name_offset);
        data_offset += files[i].data.size();
        name_offset += static_cast<std::uint32_t>(files[i].path.size() + 1);
    }
    Append(out, names);
    for (const File& f : files) Append(out, f.data);
    return out;
}

Bytes BuildRomFs(const std::vector<File>& files) {
    struct Dir {
        std::string name;
        Dir* parent = nullptr;
        std::vector<std::unique_ptr<Dir>> dirs;
        std::vector<std::pair<std::string, const Bytes*>> files;
        std::uint32_t offset = 0;
        std::vector<std::uint32_t> file_offsets;
    };
    Dir root;
    for (const File& f : files) {
        std::vector<std::string> parts = absl::StrSplit(f.path.substr(1), '/');
        Dir* dir = &root;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            auto it =
                std::find_if(dir->dirs.begin(), dir->dirs.end(), [&](const auto& d) { return d->name == parts[i]; });
            if (it == dir->dirs.end()) {
                dir->dirs.push_back(std::make_unique<Dir>());
                dir->dirs.back()->name = parts[i];
                dir->dirs.back()->parent = dir;
                it = dir->dirs.end() - 1;
            }
            dir = it->get();
        }
        dir->files.emplace_back(parts.back(), &f.data);
    }
    constexpr std::uint32_t kNone = 0xffffffff;
    auto padded = [](std::size_t n) { return static_cast<std::uint32_t>(AlignUp(n, 4)); };
    // Offsets: directories breadth first, files in their directories' order
    std::vector<Dir*> order = {&root};
    for (std::size_t i = 0; i < order.size(); ++i) {
        for (auto& d : order[i]->dirs) order.push_back(d.get());
    }
    std::uint32_t dir_size = 0;
    std::uint32_t file_size = 0;
    for (Dir* d : order) {
        d->offset = dir_size;
        dir_size += 0x18 + padded(d->name.size());
        for (const auto& [name, data] : d->files) {
            d->file_offsets.push_back(file_size);
            file_size += 0x20 + padded(name.size());
        }
    }
    Bytes dirs(dir_size, 0);
    Bytes file_table(file_size, 0);
    Bytes data;
    for (Dir* d : order) {
        PutLe<std::uint32_t>(dirs, d->offset, d->parent ? d->parent->offset : 0);
        std::uint32_t sibling = kNone;
        if (d->parent) {
            const auto& siblings = d->parent->dirs;
            for (std::size_t i = 0; i + 1 < siblings.size(); ++i) {
                if (siblings[i].get() == d) sibling = siblings[i + 1]->offset;
            }
        }
        PutLe<std::uint32_t>(dirs, d->offset + 4, sibling);
        PutLe<std::uint32_t>(dirs, d->offset + 8, d->dirs.empty() ? kNone : d->dirs.front()->offset);
        PutLe<std::uint32_t>(dirs, d->offset + 0xc, d->file_offsets.empty() ? kNone : d->file_offsets.front());
        PutLe<std::uint32_t>(dirs, d->offset + 0x10, kNone);
        PutLe<std::uint32_t>(dirs, d->offset + 0x14, static_cast<std::uint32_t>(d->name.size()));
        std::copy(d->name.begin(), d->name.end(), dirs.begin() + d->offset + 0x18);
        for (std::size_t i = 0; i < d->files.size(); ++i) {
            const std::uint32_t at = d->file_offsets[i];
            const auto& [name, bytes] = d->files[i];
            data.resize(AlignUp(data.size(), 0x10));
            PutLe<std::uint32_t>(file_table, at, d->offset);
            PutLe<std::uint32_t>(file_table, at + 4, i + 1 < d->files.size() ? d->file_offsets[i + 1] : kNone);
            PutLe<std::uint64_t>(file_table, at + 8, data.size());
            PutLe<std::uint64_t>(file_table, at + 0x10, bytes->size());
            PutLe<std::uint32_t>(file_table, at + 0x18, kNone);
            PutLe<std::uint32_t>(file_table, at + 0x1c, static_cast<std::uint32_t>(name.size()));
            std::copy(name.begin(), name.end(), file_table.begin() + at + 0x20);
            Append(data, *bytes);
        }
    }
    // Header, a one-bucket directory hash table, directories, a one-bucket file hash table, files, then the data
    Bytes out(0x50, 0);
    const Bytes empty_bucket = {0xff, 0xff, 0xff, 0xff};
    std::uint64_t at = 0x50;
    PutLe<std::uint64_t>(out, 0, 0x50);
    PutLe<std::uint64_t>(out, 0x8, at);
    PutLe<std::uint64_t>(out, 0x10, 4);
    PutLe<std::uint64_t>(out, 0x18, at += 4);
    PutLe<std::uint64_t>(out, 0x20, dirs.size());
    PutLe<std::uint64_t>(out, 0x28, at += dirs.size());
    PutLe<std::uint64_t>(out, 0x30, 4);
    PutLe<std::uint64_t>(out, 0x38, at += 4);
    PutLe<std::uint64_t>(out, 0x40, file_table.size());
    const std::uint64_t data_offset = AlignUp(at + file_table.size(), 0x200);
    PutLe<std::uint64_t>(out, 0x48, data_offset);
    Append(out, empty_bucket);
    Append(out, dirs);
    Append(out, empty_bucket);
    Append(out, file_table);
    PadTo(out, data_offset);
    Append(out, data);
    return out;
}

SectionLayout BuildSha256Section(const Bytes& data, std::uint32_t block_size) {
    SectionLayout layout;
    const Bytes table = BlockHashes(data, block_size, false);
    const std::uint64_t data_offset = AlignUp(table.size(), 0x200);
    layout.plain = table;
    PadTo(layout.plain, data_offset);
    Append(layout.plain, data);
    Bytes info(0xf8, 0);
    const Sha256Hash master = Sha256(table);
    std::copy(master.begin(), master.end(), info.begin());
    PutLe<std::uint32_t>(info, 0x20, block_size);
    PutLe<std::uint32_t>(info, 0x24, 2);
    PutLe<std::uint64_t>(info, 0x28, 0);
    PutLe<std::uint64_t>(info, 0x30, table.size());
    PutLe<std::uint64_t>(info, 0x38, data_offset);
    PutLe<std::uint64_t>(info, 0x40, data.size());
    std::copy(info.begin(), info.end(), layout.hash_data.begin());
    return layout;
}

SectionLayout BuildIvfcSection(const Bytes& data, std::uint32_t block_order) {
    const std::uint64_t block = std::uint64_t{1} << block_order;
    // Level 5 is the data, each level above it the hashes of the one below, the master hash those of level 0
    std::array<Bytes, 6> levels;
    levels[5] = data;
    for (int i = 4; i >= 0; --i) levels[i] = BlockHashes(levels[i + 1], block, true);
    const Bytes master = BlockHashes(levels[0], block, true);
    EXPECT_EQ(master.size(), 32u);
    SectionLayout layout;
    Bytes info(0xf8, 0);
    std::memcpy(info.data(), "IVFC", 4);
    PutLe<std::uint32_t>(info, 4, 0x20000);
    PutLe<std::uint32_t>(info, 8, 0x20);
    PutLe<std::uint32_t>(info, 0xc, 7);
    for (int i = 0; i < 6; ++i) {
        PadTo(layout.plain, AlignUp(layout.plain.size(), block));
        PutLe<std::uint64_t>(info, 0x10 + i * 0x18, layout.plain.size());
        PutLe<std::uint64_t>(info, 0x18 + i * 0x18, levels[i].size());
        PutLe<std::uint32_t>(info, 0x20 + i * 0x18, block_order);
        Append(layout.plain, levels[i]);
    }
    std::copy(master.begin(), master.end(), info.begin() + 0xc0);
    std::copy(info.begin(), info.end(), layout.hash_data.begin());
    return layout;
}

SectionSpec Pfs0Section(const std::vector<File>& files) {
    SectionSpec spec;
    spec.fs_type = NcaFsType::kPartitionFs;
    spec.hash_type = NcaHashType::kHierarchicalSha256;
    spec.layout = BuildSha256Section(BuildPfs0(files), 0x1000);
    return spec;
}

SectionSpec RomFsSection(const std::vector<File>& files) {
    SectionSpec spec;
    spec.layout = BuildIvfcSection(BuildRomFs(files), 12);
    return spec;
}

Bytes BuildNca(const NcaSpec& spec, const KeySet& keys) {
    constexpr std::uint64_t kHeader = 0xc00;
    const int revision = spec.key_generation > 0 ? spec.key_generation - 1 : 0;
    Bytes header(kHeader, 0);
    Bytes body;
    for (int i = 0; i < 4; ++i) {
        if (!spec.sections[i]) continue;
        const SectionSpec& s = *spec.sections[i];
        const std::uint64_t offset = kHeader + body.size();
        Bytes section = s.layout.plain;
        section.resize(AlignUp(section.size(), 0x200));
        if (s.encrypt) {
            s.encrypt(spec.content_key, offset, section);
        } else if (s.encryption_type == NcaEncryptionType::kAesCtr) {
            AesCtrTransform(spec.content_key, s.counter_upper, offset, section);
        }
        PutLe<std::uint32_t>(header, 0x240 + i * 0x10, static_cast<std::uint32_t>(offset / 0x200));
        PutLe<std::uint32_t>(header, 0x244 + i * 0x10, static_cast<std::uint32_t>((offset + section.size()) / 0x200));
        Bytes fs_header(0x200, 0);
        PutLe<std::uint16_t>(fs_header, 0, 2);
        fs_header[2] = static_cast<std::uint8_t>(s.fs_type);
        fs_header[3] = static_cast<std::uint8_t>(s.hash_type);
        fs_header[4] = static_cast<std::uint8_t>(s.encryption_type);
        std::copy(s.layout.hash_data.begin(), s.layout.hash_data.end(), fs_header.begin() + 8);
        std::copy(s.patch_info.begin(), s.patch_info.end(), fs_header.begin() + 0x100);
        PutLe<std::uint64_t>(fs_header, 0x140, s.counter_upper);
        const Sha256Hash hash = Sha256(fs_header);
        std::copy(hash.begin(), hash.end(), header.begin() + 0x280 + i * 0x20);
        std::copy(fs_header.begin(), fs_header.end(), header.begin() + 0x400 + i * 0x200);
        Append(body, section);
    }
    std::memcpy(header.data() + 0x200, spec.version == 2 ? "NCA2" : "NCA3", 4);
    header[0x205] = static_cast<std::uint8_t>(spec.content_type);
    header[0x206] = std::min<std::uint8_t>(spec.key_generation, 2);
    header[0x207] = 0;  // the application key area key
    PutLe<std::uint64_t>(header, 0x208, kHeader + body.size());
    PutLe<std::uint64_t>(header, 0x210, spec.program_id.value());
    header[0x220] = spec.key_generation > 2 ? spec.key_generation : 0;
    if (spec.rights_id) {
        std::copy(spec.rights_id->begin(), spec.rights_id->end(), header.begin() + 0x230);
    } else {
        Bytes area = PatternBytes(0x40, 7);
        std::copy(spec.content_key.begin(), spec.content_key.end(), area.begin() + 0x20);
        const Aes128Key* kak = keys.key_area_key(KeyAreaKeyKind::kApplication, revision);
        EXPECT_NE(kak, nullptr);
        if (kak != nullptr) AesEcbEncrypt(*kak, area, std::span(header.data() + 0x300, 0x40));
    }
    const AesXtsKey& header_key = *keys.header_key();
    if (spec.version == 2) {
        AesXtsEncrypt(header_key, 0, 0x200, std::span(header.data(), 0x400));
        for (int i = 0; i < 4; ++i) {
            AesXtsEncrypt(header_key, 0, 0x200, std::span(header.data() + 0x400 + i * 0x200, 0x200));
        }
    } else {
        AesXtsEncrypt(header_key, 0, 0x200, header);
    }
    Append(header, body);
    return header;
}

Aes128Key EncryptTitleKey(const KeySet& keys, const Aes128Key& title_key, int revision) {
    Aes128Key out{};
    AesEcbEncrypt(*keys.titlekek(revision), title_key, out);
    return out;
}

Bytes BuildTicket(const RightsId& rights_id, const Aes128Key& encrypted_title_key, bool common, std::uint8_t revision) {
    Bytes ticket(0x2c0, 0);
    PutLe<std::uint32_t>(ticket, 0, 0x010004);  // RSA-2048, SHA-256: the body starts at 0x140
    std::memcpy(ticket.data() + 0x140, "Root-CA00000003-XS00000020", 26);
    std::copy(encrypted_title_key.begin(), encrypted_title_key.end(), ticket.begin() + 0x180);
    ticket[0x281] = common ? 0 : 1;
    ticket[0x285] = revision;
    std::copy(rights_id.begin(), rights_id.end(), ticket.begin() + 0x2a0);
    return ticket;
}

Bytes BuildContentMeta(core::TitleId title_id, std::uint32_t version, ContentMetaType type,
                       const std::vector<ContentInfo>& contents) {
    constexpr std::size_t kExtended = 0x10;
    Bytes out(0x20 + kExtended + contents.size() * 0x38 + 0x20, 0);
    PutLe<std::uint64_t>(out, 0, title_id.value());
    PutLe<std::uint32_t>(out, 8, version);
    out[0xc] = static_cast<std::uint8_t>(type);
    PutLe<std::uint16_t>(out, 0xe, kExtended);
    PutLe<std::uint16_t>(out, 0x10, static_cast<std::uint16_t>(contents.size()));
    for (std::size_t i = 0; i < contents.size(); ++i) {
        const std::size_t at = 0x20 + kExtended + i * 0x38 + 0x20;
        std::copy(contents[i].id.begin(), contents[i].id.end(), out.begin() + static_cast<std::ptrdiff_t>(at));
        PutLe<std::uint32_t>(out, at + 0x10, static_cast<std::uint32_t>(contents[i].size));
        out[at + 0x14] = static_cast<std::uint8_t>(contents[i].size >> 32);
        out[at + 0x16] = static_cast<std::uint8_t>(contents[i].type);
        out[at + 0x17] = contents[i].id_offset;
    }
    return out;
}

Bytes BuildBucketTree(const std::vector<Bytes>& entries, const std::vector<std::uint64_t>& starts, std::uint64_t end,
                      std::uint64_t entry_size) {
    constexpr std::uint64_t kNode = 0x4000;
    const std::uint64_t per_set = (kNode - 0x10) / entry_size;
    const std::uint64_t sets = (entries.size() + per_set - 1) / per_set;
    EXPECT_LE(sets, (kNode - 0x10) / 8) << "the builder writes no L2 nodes";
    Bytes out((1 + sets) * kNode, 0);
    PutLe<std::int32_t>(out, 0, 0);
    PutLe<std::int32_t>(out, 4, static_cast<std::int32_t>(sets));
    PutLe<std::int64_t>(out, 8, static_cast<std::int64_t>(end));
    for (std::uint64_t set = 0; set < sets; ++set) {
        const std::uint64_t first = set * per_set;
        const std::uint64_t count = std::min<std::uint64_t>(per_set, entries.size() - first);
        PutLe<std::int64_t>(out, 0x10 + set * 8, static_cast<std::int64_t>(starts[first]));
        const std::uint64_t node = (1 + set) * kNode;
        PutLe<std::int32_t>(out, node, static_cast<std::int32_t>(set));
        PutLe<std::int32_t>(out, node + 4, static_cast<std::int32_t>(count));
        PutLe<std::int64_t>(out, node + 8,
                            static_cast<std::int64_t>(first + count < entries.size() ? starts[first + count] : end));
        for (std::uint64_t i = 0; i < count; ++i) {
            std::copy(entries[first + i].begin(), entries[first + i].end(),
                      out.begin() + static_cast<std::ptrdiff_t>(node + 0x10 + i * entry_size));
        }
    }
    return out;
}

SectionSpec BuildPatchSection(const Bytes& base_plain, const SectionLayout& patched, std::uint64_t chunk_size) {
    // Where each whole chunk of the base game's section is, by its bytes
    std::map<Bytes, std::uint64_t> in_base;
    for (std::uint64_t at = 0; at + chunk_size <= base_plain.size(); at += chunk_size) {
        in_base.emplace(Bytes(base_plain.begin() + static_cast<std::ptrdiff_t>(at),
                              base_plain.begin() + static_cast<std::ptrdiff_t>(at + chunk_size)),
                        at);
    }
    struct Range {
        std::uint64_t virtual_offset, physical_offset, size;
        std::uint32_t storage;
    };
    std::vector<Range> ranges;
    Bytes data;
    const Bytes& plain = patched.plain;
    for (std::uint64_t at = 0; at < plain.size(); at += chunk_size) {
        const std::uint64_t size = std::min<std::uint64_t>(chunk_size, plain.size() - at);
        Bytes chunk(plain.begin() + static_cast<std::ptrdiff_t>(at),
                    plain.begin() + static_cast<std::ptrdiff_t>(at + size));
        Range r{at, 0, size, 1};
        if (auto it = in_base.find(chunk); it != in_base.end()) {
            r.storage = 0;
            r.physical_offset = it->second;
        } else {
            r.physical_offset = data.size();
            Append(data, chunk);
        }
        // One range for consecutive chunks that are consecutive in the same storage
        if (!ranges.empty() && ranges.back().storage == r.storage &&
            ranges.back().physical_offset + ranges.back().size == r.physical_offset) {
            ranges.back().size += size;
        } else {
            ranges.push_back(r);
        }
    }
    const std::uint64_t indirect_offset = AlignUp(std::max<std::uint64_t>(data.size(), 1), 0x200);
    data.resize(indirect_offset);

    std::vector<Bytes> indirect;
    std::vector<std::uint64_t> indirect_starts;
    for (const Range& r : ranges) {
        Bytes e(0x14, 0);
        PutLe<std::uint64_t>(e, 0, r.virtual_offset);
        PutLe<std::uint64_t>(e, 8, r.physical_offset);
        PutLe<std::uint32_t>(e, 0x10, r.storage);
        indirect.push_back(std::move(e));
        indirect_starts.push_back(r.virtual_offset);
    }
    // AesCtrEx entries every 0x1810 bytes (not at chunk boundaries), generations 1, 2, 3, ...; entry 1 not encrypted
    struct ExEntry {
        std::uint64_t offset;
        bool encrypted;
        std::uint32_t generation;
    };
    std::vector<ExEntry> ex;
    for (std::uint64_t at = 0, k = 0; at < indirect_offset; at += 0x1810, ++k) {
        ex.push_back({at, k != 1, static_cast<std::uint32_t>(1 + k % 3)});
    }
    std::vector<Bytes> ex_bytes;
    std::vector<std::uint64_t> ex_starts;
    for (const ExEntry& e : ex) {
        Bytes b(0x10, 0);
        PutLe<std::uint64_t>(b, 0, e.offset);
        b[8] = e.encrypted ? 0 : 1;
        PutLe<std::uint32_t>(b, 0xc, e.generation);
        ex_bytes.push_back(std::move(b));
        ex_starts.push_back(e.offset);
    }
    const Bytes indirect_table = BuildBucketTree(indirect, indirect_starts, plain.size(), 0x14);
    const Bytes ex_table = BuildBucketTree(ex_bytes, ex_starts, indirect_offset, 0x10);
    const std::uint64_t ex_offset = indirect_offset + indirect_table.size();

    SectionSpec spec;
    spec.fs_type = NcaFsType::kRomFs;
    spec.hash_type = NcaHashType::kHierarchicalIntegrity;
    spec.encryption_type = NcaEncryptionType::kAesCtrEx;
    spec.counter_upper = 0x0000567800000009;  // secure value 0x5678, generation 9
    spec.layout.hash_data = patched.hash_data;
    spec.layout.plain = data;
    Append(spec.layout.plain, indirect_table);
    Append(spec.layout.plain, ex_table);
    auto put_tree = [&](std::size_t at, std::uint64_t offset, std::uint64_t size, std::size_t count) {
        Bytes b(0x20, 0);
        PutLe<std::uint64_t>(b, 0, offset);
        PutLe<std::uint64_t>(b, 8, size);
        std::memcpy(b.data() + 0x10, "BKTR", 4);
        PutLe<std::uint32_t>(b, 0x14, 1);
        PutLe<std::int32_t>(b, 0x18, static_cast<std::int32_t>(count));
        std::copy(b.begin(), b.end(), spec.patch_info.begin() + static_cast<std::ptrdiff_t>(at));
    };
    put_tree(0, indirect_offset, indirect_table.size(), indirect.size());
    put_tree(0x20, ex_offset, ex_table.size(), ex.size());
    const std::uint64_t upper = spec.counter_upper;
    spec.encrypt = [ex, indirect_offset, upper](const Aes128Key& key, std::uint64_t nca_offset, Bytes& section) {
        for (std::size_t k = 0; k < ex.size(); ++k) {
            const std::uint64_t end = k + 1 < ex.size() ? ex[k + 1].offset : indirect_offset;
            if (!ex[k].encrypted) continue;
            AesCtrTransform(key, (upper & 0xffffffff00000000) | ex[k].generation, nca_offset + ex[k].offset,
                            std::span(section.data() + ex[k].offset, end - ex[k].offset));
        }
        AesCtrTransform(key, upper, nca_offset + indirect_offset,
                        std::span(section.data() + indirect_offset, section.size() - indirect_offset));
    };
    return spec;
}

}  // namespace nxmac::fs::testing
