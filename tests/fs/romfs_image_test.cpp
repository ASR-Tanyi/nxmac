#include "nxmac/fs/romfs_image.h"

#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test_builders.h"

namespace nxmac::fs {
namespace {

using testing::Bytes;
using testing::PatternBytes;
using testing::ToHex;

// The image of a layout, with its files' data from the given bytes
Bytes BuildImage(const RomFsImageLayout& layout, const std::vector<Bytes>& data) {
    Bytes image(layout.size(), 0);
    const std::array<std::uint8_t, 32> header = layout.Header();
    std::copy(header.begin(), header.end(), image.begin());
    for (const RomFsImageEntry& e : layout.entries()) {
        if (!e.directory) {
            std::copy(data[e.source].begin(), data[e.source].end(), image.begin() + static_cast<std::ptrdiff_t>(e.a));
        }
    }
    const Bytes index = layout.Index();
    std::copy(index.begin(), index.end(), image.begin() + static_cast<std::ptrdiff_t>(layout.index_offset()));
    return image;
}

TEST(RomFsImageTest, MatchesNxExtractsBytes) {
    // RomFS order: /b, /a, /a/x; the image's: the root's children sorted (a, b), then a's (x)
    const std::vector<RomFs::Entry> entries = {{"/b", false, 0x100, 1}, {"/a", true, 0, 0}, {"/a/x", false, 0x200, 2}};
    absl::StatusOr<RomFsImageLayout> layout = RomFsImageLayout::Plan(entries);
    ASSERT_TRUE(layout.ok()) << layout.status();
    EXPECT_EQ(layout->index_offset(), 32u + 1 + 2);
    EXPECT_EQ(layout->size(), 35u + 4 * 24 + 3);
    EXPECT_EQ(ToHex(layout->Header()),
              "444d524f4d465331"    // DMROMFS1
              "2300000000000000"    // index at 35
              "6300000000000000"    // 99 bytes: 4 entries of 24, names "abx"
              "0400000000000000");  // 4 entries
    EXPECT_EQ(ToHex(layout->Index()),
              "0100000000000000"
              "0200000000000000"
              "00000000"
              "0000"
              "00"
              "00"  // root: children 1-2
              "0300000000000000"
              "0100000000000000"
              "00000000"
              "0100"
              "00"
              "00"  // a: child 3
              "2000000000000000"
              "0100000000000000"
              "01000000"
              "0100"
              "01"
              "00"  // b: 1 byte at 32
              "2100000000000000"
              "0200000000000000"
              "02000000"
              "0100"
              "01"
              "00"  // a/x: 2 bytes at 33
              "616278");
}

TEST(RomFsImageTest, ReadsBackWhatItPlans) {
    const std::vector<testing::File> files = {{"/Pack/z.pack", PatternBytes(300, 1)},
                                              {"/Pack/A.pack", PatternBytes(5, 2)},
                                              {"/Pack/a.pack", {}},
                                              {"/Actor/x", PatternBytes(7, 3)},
                                              {"/readme", PatternBytes(1, 4)}};
    absl::StatusOr<RomFs> romfs = RomFs::Open(MakeMemoryStorage(testing::BuildRomFs(files)));
    ASSERT_TRUE(romfs.ok()) << romfs.status();
    absl::StatusOr<RomFsImageLayout> layout = RomFsImageLayout::Plan(romfs->entries());
    ASSERT_TRUE(layout.ok()) << layout.status();
    std::vector<Bytes> data;
    for (const RomFs::Entry& e : romfs->entries()) {
        data.push_back(e.directory ? Bytes() : *ReadBytes(**romfs->OpenFile(e), 0, e.size));
    }
    absl::StatusOr<RomFsImage> image = RomFsImage::Open(MakeMemoryStorage(BuildImage(*layout, data)));
    ASSERT_TRUE(image.ok()) << image.status();
    std::vector<std::string> paths;
    for (const RomFsImageEntry& e : image->entries()) paths.push_back(e.path);
    // Breadth first, by bytes: upper case before lower case
    EXPECT_EQ(paths, (std::vector<std::string>{"", "/Actor", "/Pack", "/readme", "/Actor/x", "/Pack/A.pack",
                                               "/Pack/a.pack", "/Pack/z.pack"}));
    for (const testing::File& f : files) {
        const auto it = std::find_if(image->entries().begin(), image->entries().end(),
                                     [&](const RomFsImageEntry& e) { return e.path == f.path; });
        ASSERT_NE(it, image->entries().end()) << f.path;
        EXPECT_EQ(*ReadBytes(**image->OpenFile(*it), 0, it->b), f.data) << f.path;
    }
}

TEST(RomFsImageTest, AnEmptyRomFsIsTheRootAlone) {
    absl::StatusOr<RomFsImageLayout> layout = RomFsImageLayout::Plan({});
    ASSERT_TRUE(layout.ok());
    ASSERT_EQ(layout->entries().size(), 1u);
    absl::StatusOr<RomFsImage> image = RomFsImage::Open(MakeMemoryStorage(BuildImage(*layout, {})));
    ASSERT_TRUE(image.ok()) << image.status();
    EXPECT_EQ(image->entries().size(), 1u);
}

TEST(RomFsImageTest, RefusesNamesTheHostWouldRefuse) {
    EXPECT_FALSE(RomFsImageLayout::Plan({{"/..", false, 0, 0}}).ok());
    EXPECT_FALSE(RomFsImageLayout::Plan({{"/" + std::string(0x301, 'n'), false, 0, 0}}).ok());
    EXPECT_FALSE(RomFsImageLayout::Plan({{"/a/b", false, 0, 0}}).ok());                     // no /a before it
    EXPECT_FALSE(RomFsImageLayout::Plan({{"/a", false, 0, 0}, {"/a", false, 0, 0}}).ok());  // twice
}

TEST(RomFsImageTest, TheReaderRejectsWhatTheHostRejects) {
    const std::vector<RomFs::Entry> entries = {{"/a", false, 0, 1}, {"/b", false, 0, 1}};
    const RomFsImageLayout layout = *RomFsImageLayout::Plan(entries);
    const Bytes good = BuildImage(layout, {{1}, {2}});
    ASSERT_TRUE(RomFsImage::Open(MakeMemoryStorage(good)).ok());

    Bytes bad = good;
    bad[0] = 'X';  // magic
    EXPECT_FALSE(RomFsImage::Open(MakeMemoryStorage(bad)).ok());
    bad = good;
    bad.push_back(0);  // the index no longer ends the file
    EXPECT_FALSE(RomFsImage::Open(MakeMemoryStorage(bad)).ok());
    bad = good;
    const std::size_t names = layout.index_offset() + 3 * 24;
    std::swap(bad[names], bad[names + 1]);  // "ba": the root's children out of order
    EXPECT_FALSE(RomFsImage::Open(MakeMemoryStorage(bad)).ok());
    bad = good;
    bad[layout.index_offset()] = 2;  // the root's first child is not entry 1
    EXPECT_FALSE(RomFsImage::Open(MakeMemoryStorage(bad)).ok());
}

}  // namespace
}  // namespace nxmac::fs
