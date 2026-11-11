// Opt-in checks of nxmac::fs against a real game on your own disk; skipped unless NXMAC_TEST_KEYS (your prod.keys) and
// NXMAC_TEST_BASE (the base game's NSP) are set, with NXMAC_TEST_UPDATE (its update's NSP) and NXMAC_TEST_DLC (one DLC
// NSP) optional. Every byte of the ExeFS, the RomFS (patched when an update is given) and the DLC's RomFS is read
// through the NCAs' hash trees, so a pass means the decryption, the patching and the parsing are right for that game.
// Run it from an optimized build (a large game reads for minutes):
//   NXMAC_TEST_KEYS=~/keys/prod.keys NXMAC_TEST_BASE=game.nsp NXMAC_TEST_UPDATE=update.nsp
//   build/release/bin/fs_real_test
// Key values are never printed; the game's file names and sizes are.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/escaping.h"
#include "absl/strings/str_format.h"
#include "gtest/gtest.h"
#include "nxmac/fs/content_meta.h"
#include "nxmac/fs/keys.h"
#include "nxmac/fs/nca.h"
#include "nxmac/fs/package.h"
#include "nxmac/fs/partition_fs.h"
#include "nxmac/fs/romfs.h"

namespace nxmac::fs {
namespace {

std::optional<std::string> Env(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::string(value);
}

const char* MetaType(ContentMetaType t) {
    switch (t) {
        case ContentMetaType::kApplication:
            return "application";
        case ContentMetaType::kPatch:
            return "update";
        case ContentMetaType::kAddOnContent:
            return "add-on content";
        default:
            return "other";
    }
}

// Reads all of storage in 4 MiB pieces; the first failure, with what it was reading
absl::Status ReadWhole(const Storage& storage, const std::string& what) {
    std::vector<std::uint8_t> buffer(4 << 20);
    for (std::uint64_t at = 0; at < storage.size();) {
        const std::uint64_t n = std::min<std::uint64_t>(buffer.size(), storage.size() - at);
        if (absl::Status s = storage.Read(at, std::span(buffer.data(), n)); !s.ok()) {
            return absl::Status(s.code(), absl::StrFormat("%s at %#x: %s", what, at, s.message()));
        }
        at += n;
    }
    return absl::OkStatus();
}

class RealGameTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::optional<std::string> keys = Env("NXMAC_TEST_KEYS");
        const std::optional<std::string> base = Env("NXMAC_TEST_BASE");
        if (!keys || !base) GTEST_SKIP() << "set NXMAC_TEST_KEYS and NXMAC_TEST_BASE to check a game of yours";
        absl::StatusOr<KeySet> loaded = KeySet::Load(*keys);
        ASSERT_TRUE(loaded.ok()) << loaded.status();
        keys_ = *std::move(loaded);
        base_ = Open(*base);
        if (std::optional<std::string> update = Env("NXMAC_TEST_UPDATE")) update_ = Open(*update);
        if (std::optional<std::string> dlc = Env("NXMAC_TEST_DLC")) dlc_ = Open(*dlc);
    }

    std::optional<Package> Open(const std::string& path) {
        absl::StatusOr<Package> package = Package::OpenFile(keys_, path);
        EXPECT_TRUE(package.ok()) << package.status();
        if (!package.ok()) return std::nullopt;
        return *std::move(package);
    }

    const Nca* BaseProgram() const { return base_ ? base_->Find(NcaContentType::kProgram) : nullptr; }
    const Nca* UpdateProgram() const {
        const Nca* base = BaseProgram();
        return update_ && base ? update_->Find(NcaContentType::kProgram, base->header().program_id) : nullptr;
    }
    absl::StatusOr<StoragePtr> Section(int index) const {
        const Nca* base = BaseProgram();
        if (base == nullptr) return absl::NotFoundError("no Program NCA in the base game's NSP");
        if (update_) {
            const Nca* update = UpdateProgram();
            if (update == nullptr) {
                return absl::NotFoundError("no Program NCA of the base game's program in the update");
            }
            return OpenPatchedSection(*base, *update, index);
        }
        return base->OpenSection(index);
    }

    KeySet keys_;
    std::optional<Package> base_;
    std::optional<Package> update_;
    std::optional<Package> dlc_;
};

TEST_F(RealGameTest, EveryNcaOpensWithTheKeys) {
    for (const std::optional<Package>* p : {&base_, &update_, &dlc_}) {
        if (!*p) continue;
        for (const Package::Content& c : (*p)->contents()) {
            const NcaHeader& h = c.nca.header();
            std::printf("  %-44s type %d  program %s  key generation %d%s\n", c.file_name.c_str(),
                        static_cast<int>(h.content_type), h.program_id.ToString().c_str(), h.key_generation,
                        h.has_rights_id() ? "  (title key)" : "");
            EXPECT_TRUE(c.nca.key_status().ok()) << c.file_name << ": " << c.nca.key_status();
        }
        absl::StatusOr<std::vector<ContentMeta>> metas = (*p)->ContentMetas();
        ASSERT_TRUE(metas.ok()) << metas.status();
        for (const ContentMeta& m : *metas) {
            std::printf("  content meta: %s %s version %u, %zu contents\n", MetaType(m.type),
                        m.title_id.ToString().c_str(), m.version, m.contents.size());
        }
    }
}

TEST_F(RealGameTest, ExeFsReadsWhole) {
    absl::StatusOr<StoragePtr> code = Section(kProgramCodeSection);
    ASSERT_TRUE(code.ok()) << code.status();
    absl::StatusOr<PartitionFs> exefs = PartitionFs::Open(*code);
    ASSERT_TRUE(exefs.ok()) << exefs.status();
    ASSERT_NE(exefs->Find("main"), nullptr);
    for (const PartitionFs::Entry& e : exefs->entries()) {
        absl::StatusOr<StoragePtr> file = exefs->OpenFile(e);
        ASSERT_TRUE(file.ok()) << file.status();
        const absl::Status read = ReadWhole(**file, e.name);
        EXPECT_TRUE(read.ok()) << read;
        std::string build_id;
        if (absl::StatusOr<std::vector<std::uint8_t>> h = ReadBytes(**file, 0, std::min<std::uint64_t>(e.size, 0x50));
            h.ok() && h->size() == 0x50 && std::equal(h->begin(), h->begin() + 4, "NSO0")) {
            build_id = "  build id " +
                       absl::BytesToHexString(std::string_view(reinterpret_cast<const char*>(h->data() + 0x40), 16));
        }
        std::printf("  ExeFS/%-12s %10llu bytes%s\n", e.name.c_str(), static_cast<unsigned long long>(e.size),
                    build_id.c_str());
    }
}

TEST_F(RealGameTest, RomFsReadsWhole) {
    absl::StatusOr<StoragePtr> data = Section(kProgramDataSection);
    ASSERT_TRUE(data.ok()) << data.status();
    absl::StatusOr<RomFs> romfs = RomFs::Open(*data);
    ASSERT_TRUE(romfs.ok()) << romfs.status();
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    int failures = 0;
    for (const RomFs::Entry& e : romfs->entries()) {
        if (e.directory) continue;
        absl::StatusOr<StoragePtr> file = romfs->OpenFile(e);
        ASSERT_TRUE(file.ok()) << file.status();
        if (absl::Status s = ReadWhole(**file, e.path); !s.ok() && ++failures <= 20) ADD_FAILURE() << s;
        ++files;
        bytes += e.size;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("  RomFS%s: %llu files, %.1f MB in %.1f s (%.0f MB/s), %d failed their hash check\n",
                update_ ? " (patched by the update)" : "", static_cast<unsigned long long>(files), bytes / 1e6, seconds,
                bytes / 1e6 / std::max(seconds, 1e-9), failures);
    EXPECT_EQ(failures, 0);
}

TEST_F(RealGameTest, AddOnContentReadsWhole) {
    if (!dlc_) GTEST_SKIP() << "set NXMAC_TEST_DLC to check a DLC NSP";
    const Nca* data = dlc_->Find(NcaContentType::kPublicData);
    if (data == nullptr) data = dlc_->Find(NcaContentType::kData);
    ASSERT_NE(data, nullptr) << "no data NCA in the DLC NSP";
    absl::StatusOr<StoragePtr> section = data->OpenSection(0);
    ASSERT_TRUE(section.ok()) << section.status();
    absl::StatusOr<RomFs> romfs = RomFs::Open(*section);
    ASSERT_TRUE(romfs.ok()) << romfs.status();
    std::uint64_t files = 0;
    for (const RomFs::Entry& e : romfs->entries()) {
        if (e.directory) continue;
        ++files;
        EXPECT_TRUE(ReadWhole(**romfs->OpenFile(e), e.path).ok()) << e.path;
    }
    std::printf("  add-on content %s: %llu files\n", data->header().program_id.ToString().c_str(),
                static_cast<unsigned long long>(files));
}

}  // namespace
}  // namespace nxmac::fs
