// nsp, run in-process on a game, its update and a DLC that the test builders make with made-up keys.
#include "nsp/nsp.h"

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nsp/options.h"
#include "nxmac/fs/romfs_image.h"
#include "test_builders.h"

namespace nxmac::nsp {
namespace {

namespace fsys = std::filesystem;
using fs::testing::Bytes;
using fs::testing::File;
using fs::testing::PatternBytes;
using ::testing::HasSubstr;
using ::testing::Not;

constexpr core::TitleId kGame(0x0100000000010000);
constexpr core::TitleId kUpdate(0x0100000000010800);
constexpr core::TitleId kDlc(0x0100000000011001);

// An NSO module's first bytes: the magic, then (at 0x40) its build ID, made-up like the rest
Bytes Nso(std::uint32_t seed, std::size_t size) {
    Bytes b = PatternBytes(size, seed);
    std::memcpy(b.data(), "NSO0", 4);
    return b;
}

Bytes Nacp(const std::string& name, const std::string& version) {
    Bytes nacp(0x4000, 0);
    std::memcpy(nacp.data(), name.data(), name.size());
    std::memcpy(nacp.data() + 0x3060, version.data(), version.size());
    return nacp;
}

// The data of the RomFS ends with /Pack/Big/c.bin (the last directory's last file): 9 MiB, read in two pieces
std::vector<File> GameFiles() {
    return {{"/a.bin", PatternBytes(0x1000, 1)},
            {"/Pack/b.pack", PatternBytes(0x2345, 2)},
            {"/Pack/empty", {}},
            {"/Pack/Big/c.bin", PatternBytes(9 << 20, 3)}};
}

std::vector<File> UpdateFiles() {
    std::vector<File> files = GameFiles();
    files[1].data = PatternBytes(0x2000, 20);
    files.insert(files.begin() + 2, {"/Pack/new.pack", PatternBytes(0x777, 21)});
    return files;
}

std::vector<File> DlcFiles() {
    return {{"/Dlc/Pack/a.pack", PatternBytes(0x3000, 30)}, {"/Dlc/b", PatternBytes(9, 31)}};
}

// An NSP: the NCAs, a Meta NCA whose content meta lists them, and the NCA files named by content ID
Bytes BuildNsp(const fs::KeySet& keys, core::TitleId title, std::uint32_t version, fs::ContentMetaType type,
               const std::vector<std::pair<Bytes, fs::ContentType>>& ncas) {
    std::vector<fs::ContentInfo> infos;
    std::vector<File> files;
    for (const auto& [nca, content_type] : ncas) {
        fs::ContentInfo info;
        const fs::Sha256Hash hash = fs::Sha256(nca);
        std::copy_n(hash.begin(), info.id.size(), info.id.begin());
        info.size = nca.size();
        info.type = content_type;
        infos.push_back(info);
        files.push_back({fs::ContentIdString(info.id) + ".nca", nca});
    }
    fs::testing::NcaSpec meta;
    meta.content_type = fs::NcaContentType::kMeta;
    meta.program_id = title;
    meta.sections[0] =
        fs::testing::Pfs0Section({{"Application.cnmt", fs::testing::BuildContentMeta(title, version, type, infos)}});
    files.push_back(
        {fs::testing::ToHex(PatternBytes(16, version + 1)) + ".cnmt.nca", fs::testing::BuildNca(meta, keys)});
    return fs::testing::BuildPfs0(files);
}

void WriteFile(const std::string& path, const Bytes& bytes) {
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void WriteFile(const std::string& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

Bytes ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Every file of a DMROMFS1 image, by path
std::map<std::string, Bytes> ImageFiles(const std::string& path) {
    std::map<std::string, Bytes> files;
    absl::StatusOr<fs::StoragePtr> storage = fs::OpenFileStorage(path);
    EXPECT_TRUE(storage.ok()) << storage.status();
    if (!storage.ok()) return files;
    absl::StatusOr<fs::RomFsImage> image = fs::RomFsImage::Open(*storage);
    EXPECT_TRUE(image.ok()) << image.status();
    if (!image.ok()) return files;
    for (const fs::RomFsImageEntry& e : image->entries()) {
        if (!e.directory) files[e.path] = *fs::ReadBytes(**image->OpenFile(e), 0, e.b);
    }
    return files;
}

std::map<std::string, Bytes> AsMap(const std::vector<File>& files) {
    std::map<std::string, Bytes> map;
    for (const File& f : files) map[f.path] = f.data;
    return map;
}

struct Result {
    int code;
    std::string out;
    std::string err;
};

class NspTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = ::testing::TempDir() + "nxmac_nsp_test_" + std::to_string(::getpid());
        fsys::create_directories(dir_);
        const fs::KeySet keys = fs::testing::TestKeys();
        WriteFile(Keys(), fs::testing::TestKeysText());

        fs::testing::NcaSpec program;
        program.program_id = kGame;
        program.sections[fs::kProgramCodeSection] =
            fs::testing::Pfs0Section({{"main", Nso(1, 0x3000)}, {"main.npdm", PatternBytes(0x200, 4)}});
        program.sections[fs::kProgramDataSection] = fs::testing::RomFsSection(GameFiles());
        game_program_ = fs::testing::BuildNca(program, keys);
        game_data_plain_size_ = program.sections[fs::kProgramDataSection]->layout.plain.size();
        fs::testing::NcaSpec control;
        control.content_type = fs::NcaContentType::kControl;
        control.program_id = kGame;
        control.sections[0] = fs::testing::RomFsSection(
            {{"/control.nacp", Nacp("Test Game", "1.0.0")}, {"/icon_AmericanEnglish.dat", PatternBytes(0x900, 5)}});
        game_control_ = fs::testing::BuildNca(control, keys);
        WriteFile(Game(),
                  BuildNsp(keys, kGame, 0, fs::ContentMetaType::kApplication,
                           {{game_program_, fs::ContentType::kProgram}, {game_control_, fs::ContentType::kControl}}));

        fs::testing::NcaSpec update;
        update.program_id = kGame;
        update.key_generation = 3;  // master key revision 02
        update.content_key = {0x55, 0x66, 0x77, 0x88, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        update.sections[fs::kProgramCodeSection] =
            fs::testing::Pfs0Section({{"main", Nso(2, 0x3400)}, {"main.npdm", PatternBytes(0x200, 4)}});
        update.sections[fs::kProgramDataSection] = fs::testing::BuildPatchSection(
            program.sections[fs::kProgramDataSection]->layout.plain,
            fs::testing::BuildIvfcSection(fs::testing::BuildRomFs(UpdateFiles()), 12), 0x1000);
        fs::testing::NcaSpec update_control = control;
        update_control.sections[0] = fs::testing::RomFsSection(
            {{"/control.nacp", Nacp("Test Game", "1.1.0")}, {"/icon_AmericanEnglish.dat", PatternBytes(0x980, 6)}});
        WriteFile(Update(), BuildNsp(keys, kUpdate, 65536, fs::ContentMetaType::kPatch,
                                     {{fs::testing::BuildNca(update, keys), fs::ContentType::kProgram},
                                      {fs::testing::BuildNca(update_control, keys), fs::ContentType::kControl}}));

        fs::testing::NcaSpec dlc;
        dlc.content_type = fs::NcaContentType::kPublicData;
        dlc.program_id = kDlc;
        dlc.sections[0] = fs::testing::RomFsSection(DlcFiles());
        WriteFile(Dlc(), BuildNsp(keys, kDlc, 0, fs::ContentMetaType::kAddOnContent,
                                  {{fs::testing::BuildNca(dlc, keys), fs::ContentType::kData}}));
    }
    static void TearDownTestSuite() { fsys::remove_all(dir_); }

    static std::string Keys() { return dir_ + "/prod.keys"; }
    static std::string Game() { return dir_ + "/game.nsp"; }
    static std::string Update() { return dir_ + "/update.nsp"; }
    static std::string Dlc() { return dir_ + "/dlc.nsp"; }
    // A fresh output folder for a test
    static std::string Out(const std::string& name) {
        const std::string out = dir_ + "/" + name;
        fsys::remove_all(out);
        return out;
    }

    static Result RunCli(const std::vector<std::string>& args) {
        std::ostringstream out;
        std::ostringstream err;
        const int code = RunNsp(args, out, err, false);
        return {code, out.str(), err.str()};
    }

    static inline std::string dir_;
    static inline Bytes game_program_;
    static inline Bytes game_control_;
    static inline std::size_t game_data_plain_size_ = 0;
};

TEST_F(NspTest, InfoTellsWhatEachPackageHolds) {
    const Result r = RunCli({"info", "-k", Keys(), Game(), Update(), Dlc()});
    EXPECT_EQ(r.code, kOk) << r.err;
    EXPECT_THAT(r.out, HasSubstr("game 0100000000010000, version 0"));
    EXPECT_THAT(r.out, HasSubstr("update 0100000000010800, version 65536"));
    EXPECT_THAT(r.out, HasSubstr("add-on content 0100000000011001"));
    EXPECT_THAT(r.out, HasSubstr("Test Game, display version 1.1.0"));
    EXPECT_THAT(r.out, HasSubstr("key generation 3"));
}

TEST_F(NspTest, ExtractLaysTheUpdateOverTheGame) {
    const std::string out = Out("extract");
    const Result r = RunCli({"extract", "-k", Keys(), "-o", out, Update(), Game()});  // either order
    ASSERT_EQ(r.code, kOk) << r.err;
    EXPECT_EQ(ReadFile(out + "/ExeFS/main"), Nso(2, 0x3400));
    EXPECT_EQ(ReadFile(out + "/ExeFS/main.npdm"), PatternBytes(0x200, 4));
    EXPECT_EQ(ReadFile(out + "/control.nacp"), Nacp("Test Game", "1.1.0"));
    EXPECT_EQ(ReadFile(out + "/icon.jpg"), PatternBytes(0x980, 6));
    EXPECT_EQ(ImageFiles(out + "/RomFS.img"), AsMap(UpdateFiles()));
    EXPECT_FALSE(fsys::exists(out + "/RomFS.img.part"));
    const Bytes nso = Nso(2, 0x3400);
    EXPECT_THAT(r.out, HasSubstr("build id " + fs::testing::ToHex(std::span(nso.data() + 0x40, 16))));
    EXPECT_THAT(r.out, HasSubstr("display version: 1.1.0"));
    EXPECT_THAT(r.out, HasSubstr("update version 65536"));
}

TEST_F(NspTest, ExtractWritesAFolderTreeOnRequest) {
    const std::string out = Out("folder");
    const Result r = RunCli({"extract", "-k", Keys(), "-o", out, "--romfs-dir", Game()});
    ASSERT_EQ(r.code, kOk) << r.err;
    EXPECT_EQ(ReadFile(out + "/ExeFS/main"), Nso(1, 0x3000));
    for (const File& f : GameFiles()) EXPECT_EQ(ReadFile(out + "/RomFS" + f.path), f.data) << f.path;
    EXPECT_TRUE(fsys::is_directory(out + "/RomFS/Pack/Big"));
    EXPECT_FALSE(fsys::exists(out + "/RomFS.img"));
}

TEST_F(NspTest, OneThreadOrManyWriteTheSameImage) {
    const std::string one = Out("one");
    const std::string many = Out("many");
    ASSERT_EQ(RunCli({"extract", "-k", Keys(), "-j", "1", "-o", one, Game(), Update()}).code, kOk);
    ASSERT_EQ(RunCli({"extract", "-k", Keys(), "-j", "8", "-o", many, Game(), Update()}).code, kOk);
    EXPECT_EQ(ReadFile(one + "/RomFS.img"), ReadFile(many + "/RomFS.img"));
}

TEST_F(NspTest, DlcWritesEachAddOnContentByItsIndex) {
    const std::string out = Out("aoc");
    const Result r = RunCli({"dlc", "-k", Keys(), "-o", out, Dlc()});
    ASSERT_EQ(r.code, kOk) << r.err;
    EXPECT_EQ(ImageFiles(out + "/1.img"), AsMap(DlcFiles()));
    EXPECT_THAT(r.out, HasSubstr("add-on content 0100000000011001"));
    EXPECT_EQ(RunCli({"dlc", "-k", Keys(), "-o", out, Game()}).code, kFailed);  // a game holds no add-on content
}

TEST_F(NspTest, VerifyChecksEverythingAndSaysWhatItSkips) {
    Result r = RunCli({"verify", "-k", Keys(), Game(), Update(), Dlc()});
    EXPECT_EQ(r.code, kOk) << r.err;
    EXPECT_THAT(r.out, HasSubstr("checked"));
    EXPECT_THAT(r.out, Not(HasSubstr("not checked")));
    r = RunCli({"verify", "-k", Keys(), Update()});  // the patch, without its base game
    EXPECT_EQ(r.code, kOk) << r.err;
    EXPECT_THAT(r.out, HasSubstr("not checked"));
}

TEST_F(NspTest, DamagedDataIsNamedAndNotWritten) {
    // A byte of the game's last RomFS file, /Pack/Big/c.bin, flipped in its Program NCA
    const fs::KeySet keys = fs::testing::TestKeys();
    Bytes program = game_program_;
    const fs::Nca nca = *fs::Nca::Open(keys, fs::MakeMemoryStorage(program));
    program[nca.header().sections[fs::kProgramDataSection]->offset + game_data_plain_size_ - 2] ^= 0x10;
    const std::string damaged = dir_ + "/damaged.nsp";
    WriteFile(damaged, BuildNsp(keys, kGame, 0, fs::ContentMetaType::kApplication,
                                {{program, fs::ContentType::kProgram}, {game_control_, fs::ContentType::kControl}}));

    const std::string out = Out("damaged");
    Result r = RunCli({"extract", "-k", Keys(), "-o", out, damaged});
    EXPECT_EQ(r.code, kDamaged);
    EXPECT_THAT(r.err, HasSubstr("[damaged] RomFS/Pack/Big/c.bin"));
    EXPECT_FALSE(fsys::exists(out + "/RomFS.img"));
    EXPECT_FALSE(fsys::exists(out + "/RomFS.img.part"));

    r = RunCli({"extract", "-k", Keys(), "-o", out, "--romfs-dir", damaged});
    EXPECT_EQ(r.code, kDamaged);
    EXPECT_FALSE(fsys::exists(out + "/RomFS/Pack/Big/c.bin"));  // not left half written
    EXPECT_TRUE(fsys::exists(out + "/RomFS/Pack/b.pack"));

    r = RunCli({"verify", "-k", Keys(), damaged});
    EXPECT_EQ(r.code, kDamaged);
    EXPECT_THAT(r.err, HasSubstr("/Pack/Big/c.bin"));
}

TEST_F(NspTest, MissingKeysAreNamed) {
    // Keys without master key revision 02, which the update needs
    std::string text = fs::testing::TestKeysText();
    const std::string line = "key_area_key_application_02 = ";
    const std::size_t at = text.find(line);
    ASSERT_NE(at, std::string::npos);
    text.erase(at, text.find('\n', at) - at + 1);
    const std::string old_keys = dir_ + "/old.keys";
    WriteFile(old_keys, Bytes(text.begin(), text.end()));
    const Result r = RunCli({"extract", "-k", old_keys, "-o", Out("keys"), Game(), Update()});
    EXPECT_EQ(r.code, kMissingKeys);
    EXPECT_THAT(r.err, HasSubstr("[keys]"));
    EXPECT_THAT(r.err, HasSubstr("key_area_key_application_02"));
    EXPECT_EQ(RunCli({"info", "-k", old_keys, Update()}).code, kMissingKeys);
}

TEST_F(NspTest, SaysWhatIsWrongWithTheCommandLineOrThePackages) {
    EXPECT_EQ(RunCli({"extract", Game()}).code, kUsage);
    EXPECT_EQ(RunCli({}).code, kUsage);
    EXPECT_EQ(RunCli({"help"}).code, kOk);
    Result r = RunCli({"extract", "-k", Keys(), "-o", Out("none"), Update()});
    EXPECT_EQ(r.code, kFailed);
    EXPECT_THAT(r.err, HasSubstr("no game"));
    r = RunCli({"info", "-k", Keys(), dir_ + "/missing.nsp"});
    EXPECT_EQ(r.code, kFailed);
    r = RunCli({"info", "-k", dir_ + "/missing.keys", Game()});
    EXPECT_EQ(r.code, kFailed);
}

}  // namespace
}  // namespace nxmac::nsp
