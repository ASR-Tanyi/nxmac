// shader run in-process on the golden fixtures: what it writes and prints, and its exit codes.
#include "shader/shader.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "absl/strings/str_split.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "shader/options.h"

namespace nxmac::shader_tool {
namespace {

using ::testing::HasSubstr;

std::string Fixture(const std::string& file) { return std::string(NXMAC_SHADER_FIXTURES "/") + file; }

struct Outcome {
    int code;
    std::string out, err;
};

Outcome Shader(const std::vector<std::string>& args) {
    std::ostringstream out, err;
    const int code = RunShader(args, out, err);
    return {code, out.str(), err.str()};
}

std::string Read(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST(ShaderTest, SplitsAGame) {
    const std::filesystem::path out = std::filesystem::path(::testing::TempDir()) / "shader_tool_split";
    std::filesystem::remove_all(out);
    std::filesystem::create_directories(out / "pkg");
    std::ofstream(out / "pkg" / "stale.bin") << "old";  // the package's old files go
    const Outcome r =
        Shader({"split", "-g", "01007EF00011E000", "-o", out.string(), "-p", Fixture("profiles_fixture.tsv"), "--main",
                Fixture("cases/botw/main"), Fixture("cases/botw/rom"), "-j", "2"});
    EXPECT_EQ(r.code, kOk) << r.err;
    EXPECT_THAT(r.out, HasSubstr("shaders: 73 (compute 4, geometry 2, pixel 34, vertex 33)"));
    EXPECT_THAT(r.out, HasSubstr("progress: "));
    EXPECT_FALSE(std::filesystem::exists(out / "pkg" / "stale.bin"));
    EXPECT_TRUE(std::filesystem::exists(out / "pkg" / "vs_shader.bin"));
    EXPECT_THAT(Read(out / "shader_map.tsv"),
                HasSubstr("# kind: evidence  Breath of the Wild shaders (BNSH / BAHS in RomFS + main)"));
    for (const char* table : {"shader_facts.tsv", "shader_pairs.tsv", "pso_pairs.tsv"})
        EXPECT_TRUE(std::filesystem::exists(out / table));
}

TEST(ShaderTest, StopsOnAnUnportedArchiveAndWritesNothing) {
    const std::filesystem::path out = std::filesystem::path(::testing::TempDir()) / "shader_tool_zip";
    std::filesystem::remove_all(out);
    const Outcome r = Shader({"split", "-g", "01007EF00011E000", "-o", out.string(), "--main",
                              Fixture("cases/unported_zip/main"), Fixture("cases/unported_zip/rom")});
    EXPECT_EQ(r.code, kFailed);
    EXPECT_THAT(r.err, HasSubstr("[RED] /game.zip: a zip archive: its reader is not ported to nxmac yet"));
    EXPECT_FALSE(std::filesystem::exists(out));
}

TEST(ShaderTest, ReportsAMissingProfileOrRomFs) {
    Outcome r = Shader({"split", "-g", "0100000000000001", "-o", "x", "--main", "m", "r"});
    EXPECT_EQ(r.code, kFailed);
    EXPECT_THAT(r.err, HasSubstr("no shader profile 0100000000000001"));
    r = Shader({"split", "-g", "01007EF00011E000", "-o", "x", "--main", "m", "/nonexistent/RomFS"});
    EXPECT_EQ(r.code, kFailed);
    EXPECT_THAT(r.err, HasSubstr("no RomFS folder or DMROMFS1 image there"));
}

// The build on the made-up game of tests/dmsh/fixtures/build, its translator replayed and its Metal tools stand-ins
// (python3 scripts: dmsh_test's BuildTest holds the library to dmsh_build.py's; here, the command line around it)
TEST(ShaderTest, BuildsALibrary) {
    const char* path_env = std::getenv("PATH");
    const std::string paths = path_env ? path_env : "";
    bool python = false;  // on PATH
    for (const std::string_view dir : absl::StrSplit(paths, ':'))
        python = python || std::filesystem::exists(std::filesystem::path(dir) / "python3");
    if (!python) GTEST_SKIP() << "the stand-in tools need python3";
    const std::string fixtures = NXMAC_DMSH_FIXTURES "/build";
    const std::string old = paths.empty() ? "/usr/bin:/bin" : paths;
    setenv("PATH", (fixtures + "/bin:" + old).c_str(), 1);
    const std::filesystem::path out = std::filesystem::path(::testing::TempDir()) / "shader_tool_build";
    std::filesystem::remove_all(out);
    Outcome r = Shader({"build", "--profile", fixtures + "/profile/0100000000FA4E00", "--xlate",
                        fixtures + "/bin/dmsh_xlate", "--min-macos", "none", "-o", out.string(), "--tables",
                        (out / "tables").string(), "-j", "2", fixtures + "/pkg"});
    EXPECT_EQ(r.code, kOk) << r.err << r.out;
    EXPECT_THAT(r.out, HasSubstr("shaders 11: ok 11, \n"));
    EXPECT_THAT(r.out, HasSubstr("b-7c609af36b0f.img: 11 of 11 shaders\n"));
    EXPECT_TRUE(std::filesystem::exists(out / "b-7c609af36b0f.img"));
    EXPECT_TRUE(std::filesystem::exists(out / "tables" / "cb_extent.tsv"));
    r = Shader({"build", "--profile", (out / "nowhere").string(), "--xlate", fixtures + "/bin/dmsh_xlate", "-o",
                out.string(), fixtures + "/pkg"});
    EXPECT_EQ(r.code, kFailed);
    EXPECT_THAT(r.err, HasSubstr("[RED] "));
    EXPECT_THAT(r.err, HasSubstr("no game.tsv"));
    setenv("PATH", old.c_str(), 1);
    std::filesystem::remove_all(out);
}

TEST(ShaderTest, ProfileCommand) {
    Outcome r = Shader({"profile", "01007ef00011e000"});
    EXPECT_EQ(r.code, kOk) << r.err;
    EXPECT_THAT(r.out, HasSubstr("note      Breath of the Wild shaders"));
    EXPECT_THAT(r.out, HasSubstr("fact      8e2cc2671cae6020 {\"fout\":{\"0\":\"uint\"}}"));
    r = Shader({"profile", "0100000000000001"});
    EXPECT_EQ(r.code, kFailed);
}

TEST(ShaderTest, UsageAndHelp) {
    Outcome r = Shader({});
    EXPECT_EQ(r.code, kUsage);
    EXPECT_THAT(r.err, HasSubstr("usage: shader <command>"));
    r = Shader({"help"});
    EXPECT_EQ(r.code, kOk);
    EXPECT_THAT(r.out, HasSubstr("exit codes"));
    EXPECT_TRUE(r.err.empty());
    r = Shader({"split", "-p", "/nonexistent.tsv", "-g", "x", "-o", "o", "--main", "m", "r"});
    EXPECT_EQ(r.code, kFailed);
}

}  // namespace
}  // namespace nxmac::shader_tool
