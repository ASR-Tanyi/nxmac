// nxmac::dmsh::Build held to nxnative's dmsh_build.py on a made-up game (fixtures/build): eleven Maxwell vertex, pixel
// and compute shaders assembled by fixtures/build/src/gen.py, whose library dmsh_build.py built (expected/). The
// translator is replayed (bin/dmsh_xlate gives each job what nxnative's translator gave it, recorded in xlate/), and
// the Metal tools are stand-ins that compile nothing (bin/xcrun, metal, metallib: python3 scripts that lay a library
// out as Metal's are, so its functions can be cut). The library must be the same files, byte for byte, as
// dmsh_build.py's.
#include "nxmac/dmsh/build.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "nxmac/fs/romfs_image.h"
#include "nxmac/fs/storage.h"
#include "process.h"

namespace nxmac::dmsh {
namespace {

namespace stdfs = std::filesystem;

constexpr std::string_view kFixturesDir = NXMAC_DMSH_FIXTURES "/build";
const std::string kFixtures(kFixturesDir);  // NOLINT(bugprone-throwing-static-initialization)

std::map<std::string, std::string> ImageFiles(const std::string& path) {
    std::map<std::string, std::string> out;
    absl::StatusOr<fs::StoragePtr> storage = fs::OpenFileStorage(path);
    if (!storage.ok()) return out;
    absl::StatusOr<fs::RomFsImage> image = fs::RomFsImage::Open(*std::move(storage));
    if (!image.ok()) return out;
    for (const fs::RomFsImageEntry& e : image->entries()) {
        if (e.directory) continue;
        absl::StatusOr<fs::StoragePtr> f = image->OpenFile(e);
        if (!f.ok()) continue;
        std::string data(e.b, '\0');
        if ((*f)->Read(0, std::span(reinterpret_cast<std::uint8_t*>(data.data()), data.size())).ok())
            out[e.name] = data;
    }
    return out;
}

std::string Read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string WithoutFirstLine(const std::string& s) { return s.substr(std::min(s.size(), s.find('\n') + 1)); }

class BuildTest : public testing::Test {
protected:
    void SetUp() override {
        if (RunProcess({"python3", "-c", "pass"}).code != 0) GTEST_SKIP() << "the stand-in tools need python3";
        // the stand-in xcrun first: on a Mac it stands for Xcode's in this test
        const char* path = std::getenv("PATH");
        setenv("PATH", absl::StrCat(kFixtures, "/bin:", path ? path : "/usr/bin:/bin").c_str(), 1);
        out_ = (stdfs::path(testing::TempDir()) /
                absl::StrCat("dmsh_build_", testing::UnitTest::GetInstance()->current_test_info()->name()))
                   .string();
        stdfs::remove_all(out_);
    }
    void TearDown() override {
        if (!old_path_.empty()) setenv("PATH", old_path_.c_str(), 1);
        std::error_code ec;
        stdfs::remove_all(out_, ec);
    }
    BuildOptions Options() {
        BuildOptions o;
        o.packages = kFixtures + "/pkg";
        o.profile = kFixtures + "/profile/0100000000FA4E00";
        o.out = out_;
        o.xlate = kFixtures + "/bin/dmsh_xlate";
        o.min_macos = "";
        o.jobs = 3;
        o.print = [this](const std::string& line) { lines_ += line + "\n"; };
        return o;
    }
    // Every file of the library and its tables against dmsh_build.py's: _b_cas.txt (dmsh_build.py's own cache's
    // entries) is nxnative's alone, and a table's first line holds the time it was written
    void ExpectSameAsPython() {
        std::map<std::string, std::string> want = ImageFiles(kFixtures + "/expected/b-7c609af36b0f.img");
        std::map<std::string, std::string> got = ImageFiles(out_ + "/b-7c609af36b0f.img");
        ASSERT_EQ(want.erase("_b_cas.txt"), 1u);
        ASSERT_EQ(want.size(), 48u);
        for (const auto& [name, data] : want) {
            auto it = got.find(name);
            if (it == got.end()) {
                ADD_FAILURE() << name << " is missing";
                continue;
            }
            EXPECT_TRUE(it->second == data) << name << " differs";
        }
        EXPECT_EQ(got.size(), want.size());
        for (const char* t : {"c1_full.tsv", "cb_extent.tsv"}) {
            const std::string w = Read(kFixtures + "/expected/" + t), g = Read(out_ + "/" + t);
            EXPECT_EQ(WithoutFirstLine(g), WithoutFirstLine(w)) << t;
            EXPECT_EQ(g.substr(0, g.find(" 20")), "# chain b-7c609af36b0f inputs 9ba132c8e599") << t;
        }
    }

    std::string out_, lines_, old_path_ = std::getenv("PATH") ? std::getenv("PATH") : "";
};

TEST_F(BuildTest, LibraryIsDmshBuildPys) {
    absl::StatusOr<BuildResult> r = Build(Options());
    ASSERT_TRUE(r.ok()) << r.status() << "\n" << lines_;
    EXPECT_EQ(r->shaders, 11);
    EXPECT_EQ(r->ok, 11);
    EXPECT_NE(lines_.find("shaders 11: ok 11, \n"), std::string::npos) << lines_;
    EXPECT_NE(lines_.find("readable-tile versions (<key>_bf, dmsh_fetch.py): 3 generated, 0 failed to compile"),
              std::string::npos)
        << lines_;
    ExpectSameAsPython();
    EXPECT_FALSE(stdfs::exists(out_ + "/work-" + std::to_string(getpid())));
}

TEST_F(BuildTest, SecondBuildTakesEveryShaderFromTheCache) {
    ASSERT_TRUE(Build(Options()).ok()) << lines_;
    lines_.clear();
    absl::StatusOr<BuildResult> r = Build(Options());
    ASSERT_TRUE(r.ok()) << r.status() << "\n" << lines_;
    EXPECT_NE(lines_.find("shader cache: hit 11, rebuilt 0"), std::string::npos) << lines_;
    EXPECT_NE(lines_.find("Metal cache: mac 0 reused, 0 compiled"), std::string::npos) << lines_;
    ExpectSameAsPython();
}

TEST_F(BuildTest, MinimumMacosIsInTheConventionAndTheLibraries) {
    BuildOptions o = Options();
    o.min_macos = "15.0";
    absl::StatusOr<BuildResult> r = Build(o);
    ASSERT_TRUE(r.ok()) << r.status() << "\n" << lines_;
    std::map<std::string, std::string> want = ImageFiles(kFixtures + "/expected/b-7c609af36b0f.img");
    std::map<std::string, std::string> got = ImageFiles(out_ + "/b-7c609af36b0f.img");
    const std::string convention =
        "xcrun -sdk macosx metal -c -O1 -fpreserve-invariance -fwrapv -mmacosx-version-min=15.0; xcrun -sdk macosx "
        "metallib";
    for (const auto& [name, data] : got) {
        if (name.ends_with(".metal") || name == "inputs.tsv") {
            EXPECT_TRUE(data == want[name]) << name;
        }
        if (name.ends_with("_b.json")) {
            EXPECT_NE(data.find(convention), std::string::npos) << name;
        }
        if (name.ends_with(".metallib")) {
            EXPECT_FALSE(data == want[name]) << name << ": compiled without the minimum";
        }
    }
}

TEST_F(BuildTest, ImageOfAnotherGameStops) {
    ASSERT_TRUE(Build(Options()).ok()) << lines_;
    const std::string other = out_ + "/0100000000FA4E01";
    stdfs::copy(kFixtures + "/profile/0100000000FA4E00", other);
    BuildOptions o = Options();
    o.profile = other;
    absl::StatusOr<BuildResult> r = Build(o);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("holds the shader libraries of game 0100000000FA4E00, not 0100000000FA4E01"),
              std::string::npos)
        << r.status();
}

TEST_F(BuildTest, TranslatorOfAnotherChainStops) {
    const std::string fake = out_ + "/xlate.sh";
    stdfs::create_directories(out_);
    std::ofstream(fake) << "#!/bin/sh\necho 0123456789abcdef\n";
    stdfs::permissions(fake, stdfs::perms::owner_all);
    BuildOptions o = Options();
    o.xlate = fake;
    absl::StatusOr<BuildResult> r = Build(o);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("xlate_source: lock 657232fd201f16a1 / actual 0123456789abcdef"),
              std::string::npos)
        << r.status();
}

TEST_F(BuildTest, MissingRecordStops) {
    const std::string pkg = out_ + "/pkg";
    stdfs::create_directories(out_);
    stdfs::copy(kFixtures + "/pkg", pkg);
    std::ofstream(pkg + "/cs_test.bin", std::ios::trunc) << "";
    BuildOptions o = Options();
    o.packages = pkg;
    absl::StatusOr<BuildResult> r = Build(o);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("cs_test.bin holds no record of shader"), std::string::npos) << r.status();
    EXPECT_FALSE(stdfs::exists(out_ + "/b-7c609af36b0f.img"));
}

}  // namespace
}  // namespace nxmac::dmsh
