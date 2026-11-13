#include "nxmac/loader/modules.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace nxmac::loader {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

constexpr const char* kProfile[] = {"main", "subsdk0", "subsdk1", "sdk"};
constexpr BuildModules kRows[] = {{"aaaa0000aaaa0000aaaa0000aaaa0000", "main subsdk0 sdk"},
                                  {"bbbb0000bbbb0000bbbb0000bbbb0000", "sdk subsdk1 subsdk0 main"}};

// A modules folder whose main.fixups.tsv holds `table`
std::filesystem::path Folder(const std::string& name, const std::string& table) {
    const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / "modules_test" / name;
    std::filesystem::create_directories(dir);
    if (!table.empty()) std::ofstream(dir / "main.fixups.tsv") << table;
    return dir;
}

TEST(ProfileModulesTest, MainsBuildPicksItsRow) {
    const ModuleChoice c = ProfileModules(
        Folder("a", "# PATCH_OFF\t0x10000\n# BUILD_ID\taaaa0000aaaa0000aaaa0000aaaa0000\n"), kProfile, kRows);
    EXPECT_THAT(c.names, ElementsAre("main", "subsdk0", "sdk"));
    EXPECT_EQ(c.note, "modules of main's build aaaa0000aaaa0000: main subsdk0 sdk (the profile lists 4)");
}

TEST(ProfileModulesTest, TheProfilesOrderAndNoNoteWhenAllAreThere) {
    const ModuleChoice c =
        ProfileModules(Folder("b", "# BUILD_ID\tbbbb0000bbbb0000bbbb0000bbbb0000\n"), kProfile, kRows);
    EXPECT_THAT(c.names, ElementsAre("main", "subsdk0", "subsdk1", "sdk"));
    EXPECT_THAT(c.note, IsEmpty());
}

TEST(ProfileModulesTest, AllOfThemWithoutARowForTheBuild) {
    for (const std::string& table : {std::string("# BUILD_ID\tcccc0000cccc0000cccc0000cccc0000\n"),
                                     std::string("# PATCH_OFF\t0x10000\n"), std::string()}) {
        const ModuleChoice c = ProfileModules(Folder("c" + std::to_string(table.size()), table), kProfile, kRows);
        EXPECT_THAT(c.names, ElementsAre("main", "subsdk0", "subsdk1", "sdk")) << table;
        EXPECT_THAT(c.note, IsEmpty());
    }
    // A profile without rows ({0})
    constexpr BuildModules kNone[] = {{}};
    EXPECT_EQ(
        ProfileModules(Folder("d", "# BUILD_ID\taaaa0000aaaa0000aaaa0000aaaa0000\n"), kProfile, kNone).names.size(),
        4u);
}

TEST(ProfileModulesTest, ARowsNamesAreWholeWords) {
    constexpr const char* kTen[] = {"main", "subsdk1", "subsdk10"};
    constexpr BuildModules kRow[] = {{"aaaa0000aaaa0000aaaa0000aaaa0000", "main subsdk10"}};
    const ModuleChoice c = ProfileModules(Folder("e", "# BUILD_ID\taaaa0000aaaa0000aaaa0000aaaa0000\n"), kTen, kRow);
    EXPECT_THAT(c.names, ElementsAre("main", "subsdk10"));
}

}  // namespace
}  // namespace nxmac::loader
