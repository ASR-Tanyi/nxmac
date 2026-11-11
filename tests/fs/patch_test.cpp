// Update patches (BKTR): an update's Program NCA laid over the base game's, and the bucket trees of its tables.
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nxmac/fs/nca.h"
#include "nxmac/fs/partition_fs.h"
#include "nxmac/fs/romfs.h"
#include "section_storage.h"
#include "test_builders.h"

namespace nxmac::fs {
namespace {

using testing::Bytes;
using testing::File;
using ::testing::HasSubstr;
using testing::PatternBytes;

constexpr core::TitleId kGame(0x01007ef00011e000);

// Most files are whole 0x1000-byte blocks, so that after an inserted file the base's chunks recur at other offsets
std::vector<File> BaseFiles() {
    std::vector<File> files;
    files.reserve(25);
    for (int i = 0; i < 24; ++i) {
        files.push_back({"/Pack/f" + std::to_string(i), PatternBytes(0x1000 * (1 + i % 4), i)});
    }
    files.push_back({"/Model/odd.bfres", PatternBytes(0x1234, 50)});
    return files;
}

// The update adds a file before the others, changes two, drops one and grows one
std::vector<File> UpdateFiles() {
    std::vector<File> files = {{"/Pack/new", PatternBytes(0x1000, 60)}};
    for (const File& f : BaseFiles()) {
        if (f.path == "/Pack/f5") continue;
        files.push_back(f);
    }
    files[3].data = PatternBytes(files[3].data.size(), 61);
    files[10].data[100] ^= 0xff;
    files.back().data.resize(0x3000, 0xab);
    return files;
}

struct Built {
    Bytes base;
    Bytes update;
    testing::SectionSpec patch;
    std::size_t patched_size = 0;
};

Built BuildGame(const KeySet& keys) {
    Built built;
    testing::NcaSpec base;
    base.program_id = kGame;
    base.sections[kProgramCodeSection] = testing::Pfs0Section({{"main", PatternBytes(0x2000, 70)}});
    base.sections[kProgramDataSection] = testing::RomFsSection(BaseFiles());
    built.base = testing::BuildNca(base, keys);

    const testing::SectionLayout updated = testing::BuildIvfcSection(testing::BuildRomFs(UpdateFiles()), 12);
    built.patched_size = updated.plain.size();
    built.patch = testing::BuildPatchSection(base.sections[kProgramDataSection]->layout.plain, updated, 0x1000);
    testing::NcaSpec update;
    update.program_id = kGame;
    update.key_generation = 3;  // other keys than the base game's
    update.content_key = {0xaa, 0xbb, 0xcc, 0xdd, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    update.sections[kProgramCodeSection] = testing::Pfs0Section({{"main", PatternBytes(0x2400, 71)}});
    update.sections[kProgramDataSection] = built.patch;
    built.update = testing::BuildNca(update, keys);
    return built;
}

// Every file of the patched RomFS, by path
std::map<std::string, Bytes> ReadRomFs(const StoragePtr& section) {
    std::map<std::string, Bytes> files;
    absl::StatusOr<RomFs> romfs = RomFs::Open(section);
    EXPECT_TRUE(romfs.ok()) << romfs.status();
    if (!romfs.ok()) return files;
    for (const RomFs::Entry& e : romfs->entries()) {
        if (e.directory) continue;
        absl::StatusOr<Bytes> bytes = ReadBytes(**romfs->OpenFile(e), 0, e.size);
        EXPECT_TRUE(bytes.ok()) << e.path << ": " << bytes.status();
        if (bytes.ok()) files[e.path] = *bytes;
    }
    return files;
}

TEST(PatchTest, LaysTheUpdateOverTheBaseGame) {
    const KeySet keys = testing::TestKeys();
    const Built built = BuildGame(keys);
    // The patch carries less than the whole section: the rest comes from the base game, some of it from other offsets
    ASSERT_LT(built.patch.layout.plain.size(), built.patched_size);
    const std::span<const std::uint8_t> info(built.patch.patch_info);
    const Bytes& plain = built.patch.layout.plain;
    const std::uint64_t indirect_offset = info[0] | info[1] << 8 | info[2] << 16;
    const std::uint64_t indirect_size = info[8] | info[9] << 8 | info[10] << 16;
    const StoragePtr indirect_table =
        MakeMemoryStorage(Bytes(plain.begin() + static_cast<std::ptrdiff_t>(indirect_offset),
                                plain.begin() + static_cast<std::ptrdiff_t>(indirect_offset + indirect_size)));
    absl::StatusOr<internal::BucketTreeEntries> tree =
        internal::ReadBucketTree(*indirect_table, info.subspan(0x10, 0x10), 0x14);
    ASSERT_TRUE(tree.ok()) << tree.status();
    int from_base = 0;
    int moved = 0;
    int from_patch = 0;
    absl::StatusOr<std::vector<internal::IndirectEntry>> entries = internal::ParseIndirectEntries(*tree);
    ASSERT_TRUE(entries.ok()) << entries.status();
    for (const internal::IndirectEntry& e : *entries) {
        if (e.storage_index == 0) {
            ++from_base;
            moved += e.physical_offset != e.virtual_offset ? 1 : 0;
        } else {
            ++from_patch;
        }
    }
    EXPECT_GT(from_base, 0);
    EXPECT_GT(moved, 0);
    EXPECT_GT(from_patch, 0);

    absl::StatusOr<Nca> base = Nca::Open(keys, MakeMemoryStorage(built.base));
    absl::StatusOr<Nca> update = Nca::Open(keys, MakeMemoryStorage(built.update));
    ASSERT_TRUE(base.ok()) << base.status();
    ASSERT_TRUE(update.ok()) << update.status();
    EXPECT_EQ(update->header().sections[1]->encryption_type, NcaEncryptionType::kAesCtrEx);

    absl::StatusOr<StoragePtr> data = OpenPatchedSection(*base, *update, kProgramDataSection);
    ASSERT_TRUE(data.ok()) << data.status();
    std::map<std::string, Bytes> expected;
    for (const File& f : UpdateFiles()) expected[f.path] = f.data;
    EXPECT_EQ(ReadRomFs(*data), expected);

    // The code section is whole in the update
    absl::StatusOr<StoragePtr> code = OpenPatchedSection(*base, *update, kProgramCodeSection);
    ASSERT_TRUE(code.ok()) << code.status();
    absl::StatusOr<PartitionFs> exefs = PartitionFs::Open(*code);
    ASSERT_TRUE(exefs.ok());
    EXPECT_EQ(*ReadBytes(**exefs->OpenFile(*exefs->Find("main")), 0, 0x2400), PatternBytes(0x2400, 71));
}

TEST(PatchTest, ADamagedPatchByteFailsTheHashCheck) {
    const KeySet keys = testing::TestKeys();
    Built built = BuildGame(keys);
    absl::StatusOr<Nca> clean = Nca::Open(keys, MakeMemoryStorage(built.update));
    ASSERT_TRUE(clean.ok());
    built.update[clean->header().sections[1]->offset + 0x10] ^= 1;  // the patch data's first bytes
    absl::StatusOr<Nca> base = Nca::Open(keys, MakeMemoryStorage(built.base));
    absl::StatusOr<Nca> update = Nca::Open(keys, MakeMemoryStorage(built.update));
    ASSERT_TRUE(update.ok());
    absl::StatusOr<StoragePtr> data = OpenPatchedSection(*base, *update, kProgramDataSection);
    ASSERT_TRUE(data.ok()) << data.status();
    EXPECT_EQ(ReadBytes(**data, 0, (*data)->size()).status().code(), absl::StatusCode::kDataLoss);
}

TEST(PatchTest, TheBaseGamesKeysAreNeededToo) {
    const KeySet keys = testing::TestKeys();
    const Built built = BuildGame(keys);
    absl::StatusOr<Nca> update = Nca::Open(keys, MakeMemoryStorage(built.update));
    // The base game opened with keys that lack its key area key: its section cannot be decrypted
    absl::StatusOr<KeySet> header_only = KeySet::Parse("header_key = " + testing::ToHex(PatternBytes(32, 1)) + "\n");
    absl::StatusOr<Nca> base = Nca::Open(*header_only, MakeMemoryStorage(built.base));
    ASSERT_TRUE(base.ok());
    absl::Status s = OpenPatchedSection(*base, *update, kProgramDataSection).status();
    EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_THAT(s.message(), HasSubstr("the base game's section 1"));
}

TEST(PatchTest, ReadsFromSeveralThreadsAtOnce) {
    // Storage::Read is const and thread-safe: the tsan preset checks it on the deepest stack there is (hash checks over
    // the indirect storage over AesCtrEx and AES-CTR)
    const KeySet keys = testing::TestKeys();
    const Built built = BuildGame(keys);
    absl::StatusOr<Nca> base = Nca::Open(keys, MakeMemoryStorage(built.base));
    absl::StatusOr<Nca> update = Nca::Open(keys, MakeMemoryStorage(built.update));
    absl::StatusOr<StoragePtr> data = OpenPatchedSection(*base, *update, kProgramDataSection);
    ASSERT_TRUE(data.ok()) << data.status();
    const absl::StatusOr<Bytes> expected = ReadBytes(**data, 0, (*data)->size());
    ASSERT_TRUE(expected.ok()) << expected.status();
    std::vector<absl::StatusOr<Bytes>> got(4);
    std::vector<std::thread> threads;
    threads.reserve(got.size());
    for (auto& g : got) threads.emplace_back([&] { g = ReadBytes(**data, 0, (*data)->size()); });
    for (std::thread& t : threads) t.join();
    for (const absl::StatusOr<Bytes>& g : got) {
        ASSERT_TRUE(g.ok()) << g.status();
        EXPECT_EQ(*g, *expected);
    }
}

TEST(BucketTreeTest, StorageSizesFollowHorizon) {
    using internal::BucketTreeEntryStorageSize;
    using internal::BucketTreeNodeStorageSize;
    EXPECT_EQ(BucketTreeNodeStorageSize(0x14, 0), 0u);
    EXPECT_EQ(BucketTreeNodeStorageSize(0x14, 1), 0x4000u);
    EXPECT_EQ(BucketTreeEntryStorageSize(0x14, 818), 0x4000u);  // (0x4000 - 0x10) / 0x14 entries fit in a set
    EXPECT_EQ(BucketTreeEntryStorageSize(0x14, 819), 0x8000u);
    EXPECT_EQ(BucketTreeEntryStorageSize(0x10, 1023), 0x4000u);
    // More entry sets than an L1 node has offsets for (2046) takes an L2 node
    EXPECT_EQ(BucketTreeNodeStorageSize(0x14, 2046 * 818), 0x4000u);
    EXPECT_EQ(BucketTreeNodeStorageSize(0x14, 2047 * 818), 0x8000u);
}

TEST(BucketTreeTest, RejectsBadHeadersAndOrder) {
    EXPECT_EQ(internal::ParseBucketTreeHeader(Bytes(16, 0)).status().code(), absl::StatusCode::kInvalidArgument);
    Bytes header = {'B', 'K', 'T', 'R', 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(internal::ParseBucketTreeHeader(header).status().code(), absl::StatusCode::kUnimplemented);
    header[4] = 1;
    header[8] = 3;
    EXPECT_EQ(*internal::ParseBucketTreeHeader(header), 3);

    // Indirect entries whose virtual offsets go back
    std::vector<Bytes> entries;
    for (std::uint64_t at : {0x0, 0x200, 0x100}) {
        Bytes e(0x14, 0);
        e[0] = static_cast<std::uint8_t>(at);
        e[1] = static_cast<std::uint8_t>(at >> 8);
        entries.push_back(e);
    }
    const StoragePtr table = MakeMemoryStorage(testing::BuildBucketTree(entries, {0, 0x200, 0x100}, 0x1000, 0x14));
    absl::StatusOr<internal::BucketTreeEntries> tree = internal::ReadBucketTree(*table, header, 0x14);
    ASSERT_TRUE(tree.ok()) << tree.status();
    EXPECT_EQ(tree->end_offset, 0x1000u);
    EXPECT_EQ(internal::ParseIndirectEntries(*tree).status().code(), absl::StatusCode::kInvalidArgument);

    header[8] = 4;  // the header says 4 entries, the sets hold 3
    EXPECT_EQ(internal::ReadBucketTree(*table, header, 0x14).status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace nxmac::fs
