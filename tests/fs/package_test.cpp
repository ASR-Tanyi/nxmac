#include "nxmac/fs/package.h"

#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nxmac/fs/partition_fs.h"
#include "test_builders.h"

namespace nxmac::fs {
namespace {

using testing::Bytes;
using testing::File;
using ::testing::HasSubstr;
using testing::PatternBytes;

constexpr core::TitleId kGame(0x0100000000010000);
const RightsId kRights = {0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0x02};

ContentId Id(std::uint32_t seed) {
    ContentId id;
    const Bytes b = PatternBytes(16, seed);
    std::copy(b.begin(), b.end(), id.begin());
    return id;
}

// An application's NSP: a Program NCA under a title key (with its ticket), a Control NCA, and the Meta NCA that lists
// them
Bytes BuildNsp(const KeySet& keys, bool common_ticket) {
    testing::NcaSpec program;
    program.program_id = kGame;
    program.key_generation = 3;
    program.rights_id = kRights;
    program.sections[kProgramCodeSection] = testing::Pfs0Section({{"main", PatternBytes(0x1800, 1)}});
    const Bytes program_nca = testing::BuildNca(program, keys);

    testing::NcaSpec control;
    control.content_type = NcaContentType::kControl;
    control.program_id = kGame;
    control.sections[0] = testing::RomFsSection({{"/control.nacp", PatternBytes(0x4000, 2)}});
    const Bytes control_nca = testing::BuildNca(control, keys);

    testing::NcaSpec meta;
    meta.content_type = NcaContentType::kMeta;
    meta.program_id = kGame;
    meta.sections[0] =
        testing::Pfs0Section({{"Application_0100000000010000.cnmt",
                               testing::BuildContentMeta(kGame, 0, ContentMetaType::kApplication,
                                                         {{Id(1), program_nca.size(), ContentType::kProgram, 0},
                                                          {Id(2), control_nca.size(), ContentType::kControl, 0}})}});
    const Bytes meta_nca = testing::BuildNca(meta, keys);

    const Bytes ticket =
        testing::BuildTicket(kRights, testing::EncryptTitleKey(keys, program.content_key, 2), common_ticket, 2);
    return testing::BuildPfs0({{ContentIdString(Id(1)) + ".nca", program_nca},
                               {ContentIdString(Id(2)) + ".nca", control_nca},
                               {ContentIdString(Id(3)) + ".cnmt.nca", meta_nca},
                               {testing::ToHex(kRights) + ".tik", ticket},
                               {testing::ToHex(kRights) + ".cert", PatternBytes(0x700, 4)}});
}

TEST(PackageTest, OpensAnNspWithItsTicket) {
    const KeySet keys = testing::TestKeys();
    absl::StatusOr<Package> nsp = Package::Open(keys, MakeMemoryStorage(BuildNsp(keys, true)));
    ASSERT_TRUE(nsp.ok()) << nsp.status();
    ASSERT_EQ(nsp->contents().size(), 3u);
    EXPECT_EQ(nsp->contents()[2].content_id, ContentIdString(Id(3)));
    for (const Package::Content& c : nsp->contents()) EXPECT_TRUE(c.nca.key_status().ok()) << c.file_name;

    const Nca* program = nsp->Find(NcaContentType::kProgram);
    ASSERT_NE(program, nullptr);
    absl::StatusOr<PartitionFs> exefs = PartitionFs::Open(*program->OpenSection(kProgramCodeSection));
    ASSERT_TRUE(exefs.ok()) << exefs.status();
    EXPECT_EQ(*ReadBytes(**exefs->OpenFile(*exefs->Find("main")), 0, 0x1800), PatternBytes(0x1800, 1));
    EXPECT_NE(nsp->Find(NcaContentType::kControl, kGame), nullptr);
    EXPECT_EQ(nsp->Find(NcaContentType::kControl, core::TitleId(0x0100000000010001)), nullptr);
    EXPECT_EQ(nsp->Find(NcaContentType::kManual), nullptr);

    absl::StatusOr<std::vector<ContentMeta>> metas = nsp->ContentMetas();
    ASSERT_TRUE(metas.ok()) << metas.status();
    ASSERT_EQ(metas->size(), 1u);
    const ContentMeta& meta = (*metas)[0];
    EXPECT_EQ(meta.title_id, kGame);
    EXPECT_EQ(meta.type, ContentMetaType::kApplication);
    ASSERT_EQ(meta.contents.size(), 2u);
    EXPECT_EQ(ContentIdString(meta.contents[0].id), nsp->contents()[0].content_id);
    EXPECT_EQ(meta.contents[0].type, ContentType::kProgram);
    EXPECT_EQ(meta.contents[1].type, ContentType::kControl);
}

TEST(PackageTest, APersonalizedTicketGivesNoTitleKey) {
    const KeySet keys = testing::TestKeys();
    absl::StatusOr<Package> nsp = Package::Open(keys, MakeMemoryStorage(BuildNsp(keys, false)));
    ASSERT_TRUE(nsp.ok()) << nsp.status();
    EXPECT_THAT(nsp->Find(NcaContentType::kProgram)->key_status().message(), HasSubstr("no title key for rights ID"));
}

TEST(PackageTest, FindTakesTheLowestProgramIdUnlessOneIsNamed) {
    const KeySet keys = testing::TestKeys();
    std::vector<File> ncas;
    for (std::uint64_t id : {0x0100000000010001, 0x0100000000010000, 0x0100000000010002}) {
        testing::NcaSpec spec;
        spec.program_id = core::TitleId(id);
        spec.sections[0] = testing::Pfs0Section({{"main", PatternBytes(0x100, static_cast<std::uint32_t>(id))}});
        ncas.push_back({ContentIdString(Id(static_cast<std::uint32_t>(id))) + ".nca", testing::BuildNca(spec, keys)});
    }
    absl::StatusOr<Package> nsp = Package::Open(keys, MakeMemoryStorage(testing::BuildPfs0(ncas)));
    ASSERT_TRUE(nsp.ok()) << nsp.status();
    EXPECT_EQ(nsp->Find(NcaContentType::kProgram)->header().program_id, core::TitleId(0x0100000000010000));
    EXPECT_EQ(nsp->Find(NcaContentType::kProgram, core::TitleId(0x0100000000010002))->header().program_id,
              core::TitleId(0x0100000000010002));
}

TEST(PackageTest, NamesTheFileOfAnNcaThatDoesNotOpen) {
    const KeySet keys = testing::TestKeys();
    absl::StatusOr<Package> nsp =
        Package::Open(keys, MakeMemoryStorage(testing::BuildPfs0({{"broken.nca", PatternBytes(0x1000, 9)}})));
    ASSERT_FALSE(nsp.ok());
    EXPECT_THAT(nsp.status().message(), HasSubstr("broken.nca"));
}

}  // namespace
}  // namespace nxmac::fs
