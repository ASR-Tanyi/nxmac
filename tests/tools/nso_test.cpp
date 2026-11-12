// The nso tool, its commands run in-process on the rewriter's fixtures (tests/rewriter/fixtures).
#include "nso/nso.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nso/options.h"

namespace nxmac::nso {
namespace {

namespace fsys = std::filesystem;
using ::testing::HasSubstr;
using ::testing::Not;

// The folder of the fixtures (tests/rewriter/fixtures)
std::string Fixture(std::string_view file) { return std::string(NXMAC_REWRITER_FIXTURES "/") + std::string(file); }

struct Result {
    int code = 0;
    std::string out, err;
};

Result RunCli(const std::vector<std::string>& args) {
    std::ostringstream out, err;
    Result result;
    result.code = RunNso(args, out, err);
    result.out = out.str();
    result.err = err.str();
    return result;
}

std::string ReadFile(const fsys::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

class NsoTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fsys::path(::testing::TempDir()) /
               ("nso_test_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fsys::remove_all(dir_);
        fsys::create_directories(dir_);
        tables_ = Fixture("patches_fixture.tsv");
    }
    void TearDown() override { fsys::remove_all(dir_); }

    fsys::path dir_;
    std::string tables_;
};

TEST_F(NsoTest, RewritesModulesIntoTheFolder) {
    // Modules are named after their files: copies under the names a game's ExeFS has
    fsys::copy_file(Fixture("g.nso"), dir_ / "main");
    fsys::copy_file(Fixture("sys.nso"), dir_ / "sdk");
    const fsys::path out = dir_ / "patched" / "v0";
    const Result r = RunCli({"rewrite", "-p", tables_, "-g", "0100000000009000", "-o", out.string(),
                             (dir_ / "main").string(), (dir_ / "sdk").string()});
    ASSERT_EQ(r.code, kOk) << r.err;
    for (const char* module : {"main", "sdk"}) {
        for (const char* ext :
             {"patched.bin", "fixups.tsv", "gates.tsv", "sites.tsv", "cuts.tsv", "changed.tsv", "obs.tsv"}) {
            EXPECT_TRUE(fsys::exists(out / (std::string(module) + "." + ext))) << module << "." << ext;
        }
    }
    // main's build is registered (no match table); sdk's is not, and reports the SDK-wide patches
    EXPECT_FALSE(fsys::exists(out / "main.match.tsv"));
    EXPECT_TRUE(fsys::exists(out / "sdk.match.tsv"));
    EXPECT_THAT(ReadFile(out / "main.fixups.tsv"),
                HasSubstr("# TEST_CAPABILITY\t1\n# BUILD_ID\t480046dbad79a8218759d4f247340434\n"));
    EXPECT_THAT(ReadFile(out / "sdk.gates.tsv"), HasSubstr("\tsvc\t"));
    // The reports come in the order of the command line
    EXPECT_LT(r.out.find("main {"), r.out.find("sdk {"));
    EXPECT_THAT(r.out, HasSubstr("cuts 3 obs 2"));
}

TEST_F(NsoTest, NamesAModuleWhoseFileIsNotNamedAfterIt) {
    const Result r =
        RunCli({"rewrite", "-p", tables_, "-o", dir_.string(), "-n", "subsdk0", "--spin-list", Fixture("spin.nso")});
    ASSERT_EQ(r.code, kOk) << r.err;
    EXPECT_TRUE(fsys::exists(dir_ / "subsdk0.patched.bin"));
    EXPECT_THAT(r.out, HasSubstr("subsdk0 spin wait 0x"));
    EXPECT_THAT(r.out, HasSubstr("'spin': 9"));
}

TEST_F(NsoTest, WritesNothingForAModuleItCannotRewrite) {
    const Result r = RunCli({"rewrite", "-p", tables_, "-o", dir_.string(), "-n", "sdk", Fixture("fail.nso")});
    EXPECT_EQ(r.code, kFailed);
    EXPECT_THAT(r.err, HasSubstr("[RED] "));
    EXPECT_THAT(r.err, HasSubstr("ldr x18"));
    EXPECT_TRUE(fsys::is_empty(dir_));
    const Result missing = RunCli({"rewrite", "-p", tables_, "-o", dir_.string(), (dir_ / "nowhere").string()});
    EXPECT_EQ(missing.code, kFailed);
    EXPECT_THAT(missing.err, HasSubstr("nowhere"));
    const Result not_a_module = RunCli({"rewrite", "-p", tables_, "-o", dir_.string(), tables_});
    EXPECT_EQ(not_a_module.code, kFailed);
    EXPECT_THAT(not_a_module.err, HasSubstr("not an NSO or NRO"));
}

TEST_F(NsoTest, TwoModulesOfOneNameAreAWrongCommandLine) {
    fsys::create_directories(dir_ / "a");
    fsys::create_directories(dir_ / "b");
    fsys::copy_file(Fixture("sys.nso"), dir_ / "a" / "main");
    fsys::copy_file(Fixture("excl.nso"), dir_ / "b" / "main");
    const Result r = RunCli(
        {"rewrite", "-o", (dir_ / "out").string(), (dir_ / "a" / "main").string(), (dir_ / "b" / "main").string()});
    EXPECT_EQ(r.code, kUsage);
    EXPECT_THAT(r.err, HasSubstr("two modules are named main"));
}

TEST_F(NsoTest, InfoSaysWhatAModuleIs) {
    const Result r = RunCli({"info", "-p", tables_, Fixture("g.nso"), Fixture("sys.nso"), Fixture("botw_sites.nso")});
    ASSERT_EQ(r.code, kOk) << r.err;
    EXPECT_THAT(r.out, HasSubstr("build ID 480046dbad79a8218759d4f247340434"));
    EXPECT_THAT(r.out, HasSubstr("patch tables: 3 cuts, 2 observation gates, TEST_CAPABILITY, ANOTHER_ONE"));
    EXPECT_THAT(r.out, HasSubstr("the build is not registered"));
    EXPECT_THAT(r.out, HasSubstr("7 cuts, 0 observation gates, frame-step stubs (profile 01007EF00011E000)"));
    EXPECT_THAT(r.out, HasSubstr("text at 0x0, 0x41e80 bytes"));
}

TEST_F(NsoTest, HelpAndUsage) {
    const Result help = RunCli({"help"});
    EXPECT_EQ(help.code, kOk);
    EXPECT_THAT(help.out, HasSubstr("usage: nso <command>"));
    const Result wrong = RunCli({"rewrite"});
    EXPECT_EQ(wrong.code, kUsage);
    EXPECT_THAT(wrong.err, HasSubstr("needs at least one module"));
    EXPECT_THAT(wrong.err, HasSubstr("usage: nso"));
    const Result tables = RunCli({"info", "-p", (dir_ / "none.tsv").string(), Fixture("sys.nso")});
    EXPECT_EQ(tables.code, kFailed);
    EXPECT_THAT(tables.err, Not(HasSubstr("usage:")));
}

}  // namespace
}  // namespace nxmac::nso
