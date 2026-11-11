#include "nxmac/fs/nca.h"

#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nxmac/fs/partition_fs.h"
#include "nxmac/fs/romfs.h"
#include "test_builders.h"

namespace nxmac::fs {
namespace {

using ::testing::AllOf;
using testing::Bytes;
using testing::File;
using ::testing::HasSubstr;
using testing::PatternBytes;

std::vector<File> CodeFiles() { return {{"main", PatternBytes(0x5123, 1)}, {"main.npdm", PatternBytes(0x300, 2)}}; }

std::vector<File> DataFiles() {
    std::vector<File> files;
    files.reserve(13);
    for (int i = 0; i < 12; ++i) {
        files.push_back(
            {"/Pack/file" + std::to_string(i) + ".bin", PatternBytes(static_cast<std::size_t>(i) * 0x777, 10 + i)});
    }
    files.push_back({"/System/Version.txt", PatternBytes(0x20000, 99)});
    return files;
}

testing::NcaSpec ProgramSpec() {
    testing::NcaSpec spec;
    spec.program_id = core::TitleId(0x01007ef00011e000);
    spec.sections[kProgramCodeSection] = testing::Pfs0Section(CodeFiles());
    spec.sections[kProgramDataSection] = testing::RomFsSection(DataFiles());
    return spec;
}

// Reads every file of the code (PFS0) and data (RomFS) sections and compares them with what was built
void ExpectProgramContent(const Nca& nca) {
    absl::StatusOr<StoragePtr> code = nca.OpenSection(kProgramCodeSection);
    ASSERT_TRUE(code.ok()) << code.status();
    absl::StatusOr<PartitionFs> exefs = PartitionFs::Open(*code);
    ASSERT_TRUE(exefs.ok()) << exefs.status();
    for (const File& f : CodeFiles()) {
        const PartitionFs::Entry* e = exefs->Find(f.path);
        ASSERT_NE(e, nullptr) << f.path;
        EXPECT_EQ(*ReadBytes(**exefs->OpenFile(*e), 0, e->size), f.data) << f.path;
    }
    absl::StatusOr<StoragePtr> data = nca.OpenSection(kProgramDataSection);
    ASSERT_TRUE(data.ok()) << data.status();
    absl::StatusOr<RomFs> romfs = RomFs::Open(*data);
    ASSERT_TRUE(romfs.ok()) << romfs.status();
    std::size_t files = 0;
    for (const RomFs::Entry& e : romfs->entries()) {
        if (e.directory) continue;
        ++files;
        absl::StatusOr<StoragePtr> file = romfs->OpenFile(e);
        ASSERT_TRUE(file.ok()) << file.status();
        absl::StatusOr<Bytes> bytes = ReadBytes(**file, 0, e.size);
        ASSERT_TRUE(bytes.ok()) << e.path << ": " << bytes.status();
        bool found = false;
        for (const File& f : DataFiles()) found = found || (f.path == e.path && f.data == *bytes);
        EXPECT_TRUE(found) << e.path;
    }
    EXPECT_EQ(files, DataFiles().size());
}

class NcaTest : public ::testing::Test {
protected:
    absl::StatusOr<Nca> Open(const Bytes& bytes) { return Nca::Open(keys_, MakeMemoryStorage(bytes)); }

    KeySet keys_ = testing::TestKeys();
};

TEST_F(NcaTest, ReadsAProgramsCodeAndData) {
    absl::StatusOr<Nca> nca = Open(testing::BuildNca(ProgramSpec(), keys_));
    ASSERT_TRUE(nca.ok()) << nca.status();
    const NcaHeader& h = nca->header();
    EXPECT_EQ(h.version, 3);
    EXPECT_EQ(h.content_type, NcaContentType::kProgram);
    EXPECT_EQ(h.program_id, core::TitleId(0x01007ef00011e000));
    EXPECT_FALSE(h.has_rights_id());
    EXPECT_TRUE(nca->has_section(0));
    EXPECT_TRUE(nca->has_section(1));
    EXPECT_FALSE(nca->has_section(2));
    EXPECT_EQ(h.sections[1]->hash_type, NcaHashType::kHierarchicalIntegrity);
    EXPECT_TRUE(nca->key_status().ok()) << nca->key_status();
    ExpectProgramContent(*nca);
    EXPECT_EQ(nca->OpenSection(2).status().code(), absl::StatusCode::kNotFound);
}

TEST_F(NcaTest, ReadsNca2Headers) {
    testing::NcaSpec spec = ProgramSpec();
    spec.version = 2;
    absl::StatusOr<Nca> nca = Open(testing::BuildNca(spec, keys_));
    ASSERT_TRUE(nca.ok()) << nca.status();
    EXPECT_EQ(nca->header().version, 2);
    ExpectProgramContent(*nca);
}

TEST_F(NcaTest, TakesTheKeysOfItsKeyGeneration) {
    testing::NcaSpec spec = ProgramSpec();
    spec.key_generation = 4;  // the header's second field; master key revision 3
    absl::StatusOr<Nca> nca = Open(testing::BuildNca(spec, keys_));
    ASSERT_TRUE(nca.ok()) << nca.status();
    EXPECT_EQ(nca->header().key_generation, 4);
    EXPECT_EQ(nca->header().master_key_revision(), 3);
    ExpectProgramContent(*nca);
}

TEST_F(NcaTest, NamesTheKeyAreaKeyProdKeysLacks) {
    absl::StatusOr<KeySet> newer = KeySet::Parse(
        testing::TestKeysText() + "key_area_key_application_05 = " + testing::ToHex(PatternBytes(16, 105)) + "\n");
    ASSERT_TRUE(newer.ok());
    testing::NcaSpec spec = ProgramSpec();
    spec.key_generation = 6;  // master key revision 05, which the test keys lack
    absl::StatusOr<Nca> nca = Open(testing::BuildNca(spec, *newer));
    ASSERT_TRUE(nca.ok()) << nca.status();  // the header still reads
    EXPECT_EQ(nca->key_status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_THAT(nca->key_status().message(), AllOf(HasSubstr("key_area_key_application_05"), HasSubstr("master key 05"),
                                                   HasSubstr("firmware 6.0.0")));
    EXPECT_EQ(nca->OpenSection(1).status(), nca->key_status());
    ExpectProgramContent(*Nca::Open(*newer, MakeMemoryStorage(testing::BuildNca(spec, *newer))));
}

TEST_F(NcaTest, OpensWithATicketsTitleKey) {
    testing::NcaSpec spec = ProgramSpec();
    spec.key_generation = 3;
    spec.rights_id = RightsId{0x01, 0x00, 0x7e, 0xf0, 0x00, 0x11, 0xe0, 0x00, 0, 0, 0, 0, 0, 0, 0, 0x02};
    const Bytes bytes = testing::BuildNca(spec, keys_);

    absl::StatusOr<Nca> without = Open(bytes);
    ASSERT_TRUE(without.ok()) << without.status();
    EXPECT_TRUE(without->header().has_rights_id());
    EXPECT_THAT(without->key_status().message(),
                HasSubstr("no title key for rights ID 01007ef00011e0000000000000000002"));

    keys_.AddTitleKey(*spec.rights_id, testing::EncryptTitleKey(keys_, spec.content_key, 2));
    absl::StatusOr<Nca> with = Open(bytes);
    ASSERT_TRUE(with.ok()) << with.status();
    EXPECT_TRUE(with->key_status().ok()) << with->key_status();
    ExpectProgramContent(*with);
}

TEST_F(NcaTest, AnotherHeaderKeyReadsNoNca) {
    absl::StatusOr<KeySet> other = KeySet::Parse("header_key = " + testing::ToHex(PatternBytes(32, 77)) + "\n");
    ASSERT_TRUE(other.ok());
    absl::StatusOr<Nca> nca = Nca::Open(*other, MakeMemoryStorage(testing::BuildNca(ProgramSpec(), keys_)));
    EXPECT_EQ(nca.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(Nca::Open(KeySet(), MakeMemoryStorage(Bytes(0x1000))).status().code(),
              absl::StatusCode::kFailedPrecondition);  // no header_key at all
}

TEST_F(NcaTest, ADamagedSectionHeaderFailsOpen) {
    Bytes bytes = testing::BuildNca(ProgramSpec(), keys_);
    bytes[0x400 + 0x50] ^= 1;  // inside section 0's header, encrypted
    absl::StatusOr<Nca> nca = Open(bytes);
    EXPECT_EQ(nca.status().code(), absl::StatusCode::kDataLoss);
    EXPECT_THAT(nca.status().message(), HasSubstr("section 0"));
}

TEST_F(NcaTest, ACutShortNcaFailsOpen) {
    Bytes bytes = testing::BuildNca(ProgramSpec(), keys_);
    bytes.resize(bytes.size() - 0x200);
    EXPECT_EQ(Open(bytes).status().code(), absl::StatusCode::kDataLoss);
}

TEST_F(NcaTest, ADamagedDataByteFailsItsHashCheck) {
    const testing::NcaSpec spec = ProgramSpec();
    const Bytes good = testing::BuildNca(spec, keys_);
    absl::StatusOr<Nca> nca = Open(good);
    ASSERT_TRUE(nca.ok());
    const NcaSection& data = *nca->header().sections[1];

    // A byte of the RomFS's last file (its last block): that file fails, the first still reads
    Bytes bad = good;
    bad[data.offset + spec.sections[1]->layout.plain.size() - 2] ^= 0x40;
    absl::StatusOr<Nca> damaged = Open(bad);
    ASSERT_TRUE(damaged.ok());
    absl::StatusOr<RomFs> romfs = RomFs::Open(*damaged->OpenSection(1));
    ASSERT_TRUE(romfs.ok()) << romfs.status();
    const RomFs::Entry& last = romfs->entries().back();
    const absl::Status read = ReadBytes(**romfs->OpenFile(last), 0, last.size).status();
    EXPECT_EQ(read.code(), absl::StatusCode::kDataLoss) << read;
    EXPECT_THAT(read.message(), HasSubstr("IVFC data"));
    const RomFs::Entry& early = romfs->entries()[2];  // /Pack/file1.bin, after the /Pack directory and an empty file
    ASSERT_EQ(early.path, "/Pack/file1.bin");
    EXPECT_TRUE(ReadBytes(**romfs->OpenFile(early), 0, early.size).ok());

    // A byte of the code section's hash table: the master hash fails when the section opens
    bad = good;
    bad[nca->header().sections[0]->offset + 3] ^= 1;
    damaged = Open(bad);
    ASSERT_TRUE(damaged.ok());
    EXPECT_EQ(damaged->OpenSection(0).status().code(), absl::StatusCode::kDataLoss);
}

TEST_F(NcaTest, AnUpdatePatchSectionNeedsItsBase) {
    testing::NcaSpec spec;
    spec.sections[1] = testing::RomFsSection(DataFiles());
    spec.sections[1]->encryption_type = NcaEncryptionType::kAesCtrEx;
    absl::StatusOr<Nca> nca = Open(testing::BuildNca(spec, keys_));
    ASSERT_TRUE(nca.ok()) << nca.status();
    EXPECT_EQ(nca->OpenSection(1).status().code(), absl::StatusCode::kFailedPrecondition);
}

}  // namespace
}  // namespace nxmac::fs
