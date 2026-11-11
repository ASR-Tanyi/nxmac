#include "nxmac/fs/partition_fs.h"

#include <cstring>

#include "gtest/gtest.h"
#include "test_builders.h"

namespace nxmac::fs {
namespace {

using testing::Bytes;
using testing::PatternBytes;

Bytes ReadAll(const PartitionFs& fs, const PartitionFs::Entry& e) {
    absl::StatusOr<StoragePtr> file = fs.OpenFile(e);
    EXPECT_TRUE(file.ok()) << file.status();
    absl::StatusOr<Bytes> bytes = ReadBytes(**file, 0, (*file)->size());
    EXPECT_TRUE(bytes.ok()) << bytes.status();
    return bytes.ok() ? *bytes : Bytes();
}

TEST(PartitionFsTest, ListsAndReadsItsFiles) {
    const std::vector<testing::File> files = {{"main", PatternBytes(5000, 1)},
                                              {"main.npdm", PatternBytes(300, 2)},
                                              {"empty", {}},
                                              {"sdk", PatternBytes(77, 3)}};
    absl::StatusOr<PartitionFs> fs = PartitionFs::Open(MakeMemoryStorage(testing::BuildPfs0(files)));
    ASSERT_TRUE(fs.ok()) << fs.status();
    ASSERT_EQ(fs->entries().size(), files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        EXPECT_EQ(fs->entries()[i].name, files[i].path);
        EXPECT_EQ(ReadAll(*fs, fs->entries()[i]), files[i].data) << files[i].path;
    }
    ASSERT_NE(fs->Find("main.npdm"), nullptr);
    EXPECT_EQ(fs->Find("main.npdm")->size, 300u);
    EXPECT_EQ(fs->Find("rtld"), nullptr);
}

TEST(PartitionFsTest, ReadsALastNameWithoutItsNul) {
    // As older NSZ writers left it: one file "ab", a name table of exactly "ab"
    Bytes pfs(0x10 + 0x18, 0);
    std::memcpy(pfs.data(), "PFS0", 4);
    pfs[4] = 1;
    pfs[8] = 2;
    pfs[0x18] = 3;  // the file's size
    pfs.insert(pfs.end(), {'a', 'b', 'x', 'y', 'z'});
    absl::StatusOr<PartitionFs> fs = PartitionFs::Open(MakeMemoryStorage(pfs));
    ASSERT_TRUE(fs.ok()) << fs.status();
    ASSERT_EQ(fs->entries().size(), 1u);
    EXPECT_EQ(fs->entries()[0].name, "ab");
    EXPECT_EQ(ReadAll(*fs, fs->entries()[0]), (Bytes{'x', 'y', 'z'}));
}

TEST(PartitionFsTest, RejectsWhatIsNotOne) {
    EXPECT_EQ(PartitionFs::Open(MakeMemoryStorage(Bytes(0x40, 0))).status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(PartitionFs::Open(MakeMemoryStorage(Bytes(4, 0))).status().code(), absl::StatusCode::kInvalidArgument);

    Bytes pfs = testing::BuildPfs0({{"a", PatternBytes(100, 1)}});
    Bytes cut(pfs.begin(), pfs.end() - 1);  // the file's data ends past the end
    EXPECT_EQ(PartitionFs::Open(MakeMemoryStorage(cut)).status().code(), absl::StatusCode::kInvalidArgument);

    pfs[4] = 0xff;  // 255 entries: the tables do not fit
    EXPECT_EQ(PartitionFs::Open(MakeMemoryStorage(pfs)).status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace nxmac::fs
