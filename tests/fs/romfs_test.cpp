#include "nxmac/fs/romfs.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test_builders.h"

namespace nxmac::fs {
namespace {

using testing::Bytes;
using testing::PatternBytes;

TEST(RomFsTest, ListsDirectoriesAndFilesDepthFirst) {
    const std::vector<testing::File> files = {
        {"/a.txt", PatternBytes(10, 1)}, {"/dir/b.bin", PatternBytes(5000, 2)}, {"/dir/sub/c", {}},
        {"/dir/d", PatternBytes(16, 3)}, {"/other/e", PatternBytes(1, 4)},
    };
    absl::StatusOr<RomFs> fs = RomFs::Open(MakeMemoryStorage(testing::BuildRomFs(files)));
    ASSERT_TRUE(fs.ok()) << fs.status();
    std::vector<std::string> listed;
    for (const RomFs::Entry& e : fs->entries()) listed.push_back(e.directory ? e.path + "/" : e.path);
    EXPECT_EQ(listed, (std::vector<std::string>{"/a.txt", "/dir/", "/dir/b.bin", "/dir/d", "/dir/sub/", "/dir/sub/c",
                                                "/other/", "/other/e"}));
    for (const testing::File& f : files) {
        const auto it = std::find_if(fs->entries().begin(), fs->entries().end(),
                                     [&](const RomFs::Entry& e) { return e.path == f.path; });
        ASSERT_NE(it, fs->entries().end()) << f.path;
        absl::StatusOr<StoragePtr> file = fs->OpenFile(*it);
        ASSERT_TRUE(file.ok()) << file.status();
        EXPECT_EQ(*ReadBytes(**file, 0, (*file)->size()), f.data) << f.path;
    }
}

TEST(RomFsTest, AnEmptyRomFsHasNoEntries) {
    absl::StatusOr<RomFs> fs = RomFs::Open(MakeMemoryStorage(testing::BuildRomFs({})));
    ASSERT_TRUE(fs.ok()) << fs.status();
    EXPECT_TRUE(fs->entries().empty());
}

TEST(RomFsTest, RejectsLinksThatGoRoundInACircle) {
    Bytes image = testing::BuildRomFs({{"/dir/x", PatternBytes(4, 1)}});
    // The directory table starts at 0x54; /dir is the entry after the root's (0x18 bytes, an empty name): its sibling
    // is made itself
    const std::size_t dir_entry = 0x54 + 0x18;
    image[dir_entry + 4] = 0x18;
    image[dir_entry + 5] = image[dir_entry + 6] = image[dir_entry + 7] = 0;
    EXPECT_EQ(RomFs::Open(MakeMemoryStorage(image)).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RomFsTest, RejectsFileDataOutsideTheImage) {
    Bytes image = testing::BuildRomFs({{"/x", PatternBytes(4, 1)}});
    image.resize(image.size() - 1);
    EXPECT_EQ(RomFs::Open(MakeMemoryStorage(image)).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(RomFsTest, RejectsWhatIsNotOne) {
    EXPECT_FALSE(RomFs::Open(MakeMemoryStorage(Bytes(0x100, 0))).ok());
    EXPECT_FALSE(RomFs::Open(MakeMemoryStorage(Bytes(0x10, 0))).ok());
}

}  // namespace
}  // namespace nxmac::fs
