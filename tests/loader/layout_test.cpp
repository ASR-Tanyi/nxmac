#include "nxmac/loader/layout.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nxmac/rewriter/module.h"
#include "nxmac/rewriter/rewrite.h"

namespace nxmac::loader {
namespace {

using ::testing::HasSubstr;

// The keys every table has, as nso writes them
constexpr std::string_view kKeys = "# KEY_K18_IDX\t0\n# KEY_KTLS_IDX\t1\n# KEY_KTLR_IDX\t2\n# KEY_KGATE_IDX\t3\n";

std::string Plain(std::string_view more = "") {
    return absl::StrCat("# PATCH_OFF\t0x10000\n# KEY_PAGE_OFF\t0xc000\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t0x6000\n",
                        "# RO_OFF\t0x8000\n# DATA_OFF\t0x9000\n", kKeys, more);
}

// A far module's table: 128 MiB of image, the patch area 32 MiB before the base
std::string Far(std::string_view more = "") {
    return absl::StrCat("# PATCH_OFF\t-0x2000000\n# KEY_PAGE_OFF\t0x8000000\n# KEY_PAGE_SIZE\t0x4000\n",
                        "# TEXT_END\t0x6000\n# RO_OFF\t0x8000\n# DATA_OFF\t0x9000\n# IMAGE_SIZE\t0x8008000\n",
                        "# FILE_IMAGE_LEN\t0xc000\n", more, kKeys);
}

TEST(LayoutTest, ReadsWhatNsoWrites) {
    const absl::StatusOr<ModuleLayout> m =
        ParseLayout(Plain("# FPS_STATE_OFF\t0x154c000\n# RENDER_SCALE\t1\n# FRAME_STEP\t1\n"
                          "# BUILD_ID\tcd57b23fa4bbad65803d9788c01821ee\n"),
                    "main.fixups.tsv");
    ASSERT_TRUE(m.ok()) << m.status();
    EXPECT_EQ(m->patch_off, 0x10000);
    EXPECT_EQ(m->key_page_off, 0xc000u);
    EXPECT_EQ(m->key_page_size, kPageSize);
    EXPECT_EQ(m->text_end, 0x6000u);
    EXPECT_EQ(m->ro_off, 0x8000u);
    EXPECT_EQ(m->data_off, 0x9000u);
    EXPECT_EQ(m->fps_state_off, 0x154c000u);
    EXPECT_EQ(m->render_scale_max, 1u);
    EXPECT_EQ(m->build_id, "cd57b23fa4bbad65803d9788c01821ee");
    EXPECT_EQ(m->key_idx, (std::array<unsigned, kKeyCount>{0, 1, 2, 3}));
    EXPECT_EQ(m->image_size, 0u);
    EXPECT_EQ(m->back_off, 0u);
}

TEST(LayoutTest, AFarModuleHasItsPatchAreaBeforeTheBase) {
    const absl::StatusOr<ModuleLayout> m = ParseLayout(Far(), "main.fixups.tsv");
    ASSERT_TRUE(m.ok()) << m.status();
    EXPECT_EQ(m->patch_off, -0x2000000);
    EXPECT_EQ(m->image_size, 0x8008000u);
    EXPECT_EQ(m->file_image_len, 0xc000u);
    // ... and a back patch area at the image's end (TWO)
    const absl::StatusOr<ModuleLayout> two =
        ParseLayout(Far("# BACK_OFF\t0x8004000\n# BACK_LEN\t0x4000\n"), "main.fixups.tsv");
    ASSERT_TRUE(two.ok()) << two.status();
    EXPECT_EQ(two->back_off, 0x8004000u);
    EXPECT_EQ(two->back_len, 0x4000u);
}

TEST(LayoutTest, ReadsTheFindersValues) {
    const absl::StatusOr<ModuleLayout> m =
        ParseLayout(Plain("# UE_MALLOC_VT\t0x1234\n# UE_MALLOC_SLOT\t5\n# UE_CVAR:r.ScreenPercentage\t0x4400\n"
                          "# FPS_TARGET_PTR\t0x100\n# FPS_TARGET_FIELD\t0x18\n# UE_FIXED_RATE_PTR\t0x200\n"
                          "# UE_FIXED_RATE_FIELD\t0x2c\n# UE_FIXED_RATE_FLAG\t0x3004\n# UNITY_TARGET_FPS\t0x300\n"
                          "# UNITY_QS_PTR\t0x400\n# UNITY_QS_IDX\t0x10\n# UNITY_QS_ARR\t0x20\n# UNITY_QS_SIZE\t0x30\n"
                          "# UNITY_QS_VSYNC\t0x4\n"),
                    "main.fixups.tsv");
    ASSERT_TRUE(m.ok()) << m.status();
    EXPECT_EQ(m->ue_malloc_vt, 0x1234u);
    EXPECT_EQ(m->ue_malloc_slot, 5u);
    ASSERT_EQ(m->ue_cvars.size(), 1u);
    EXPECT_EQ(m->ue_cvars[0].name, "r.ScreenPercentage");
    EXPECT_EQ(m->ue_cvars[0].off, 0x4400u);
    EXPECT_EQ(m->fps_target_ptr, 0x100u);
    EXPECT_EQ(m->fps_target_field, 0x18u);
    EXPECT_EQ(m->ue_fixed_ptr, 0x200u);
    EXPECT_EQ(m->ue_fixed_field, 0x2cu);
    EXPECT_EQ(m->ue_fixed_flag, 0x3004u);
    EXPECT_EQ(m->unity_target_fps, 0x300u);
    EXPECT_EQ(m->unity_qs_ptr, 0x400u);
    EXPECT_EQ(m->unity_qs_idx, 0x10u);
    EXPECT_EQ(m->unity_qs_arr, 0x20u);
    EXPECT_EQ(m->unity_qs_size, 0x30u);
    EXPECT_EQ(m->unity_qs_vsync, 0x4u);
}

TEST(LayoutTest, AKeyIndexIsTheDecimalAfterTheLastTab) {
    std::string text = Plain();
    text.replace(text.find("# KEY_KGATE_IDX\t3"), 17, "# KEY_KGATE_IDX\t10");
    const absl::StatusOr<ModuleLayout> m = ParseLayout(text, "main.fixups.tsv");
    ASSERT_TRUE(m.ok()) << m.status();
    EXPECT_EQ(m->key_idx[kKeyGate], 10u);  // 10, not 0x10
}

TEST(LayoutTest, AnOldRewritersTableStops) {
    constexpr std::string_view kOld = "the output comes from an old rewriter";
    // A line that is not "# KEY value": an empty one, a key without a number, a table line. (As with nxnative's sscanf,
    // "# a comment" would pass: %lx reads the c of "comment" as 0xc.)
    for (const std::string& text : {Plain("\n"), Plain("# note\n"), Plain("0x1000\tsvc\n"), std::string()}) {
        const absl::StatusOr<ModuleLayout> m = ParseLayout(text, "sdk.fixups.tsv");
        EXPECT_EQ(m.status().code(), absl::StatusCode::kInvalidArgument);
        EXPECT_THAT(m.status().message(), HasSubstr(kOld));
        EXPECT_THAT(m.status().message(), HasSubstr("sdk.fixups.tsv: "));
    }
    // What the loader needs: a patch area, a key page of one page, every key index
    std::string no_gate = Plain();
    no_gate.erase(no_gate.find("# KEY_KGATE_IDX"), 18);
    std::string odd_page = Plain();
    odd_page.replace(odd_page.find("# KEY_PAGE_OFF\t0xc000"), 21, "# KEY_PAGE_OFF\t0xc100");
    std::string no_patch = Plain();
    no_patch.replace(no_patch.find("# PATCH_OFF\t0x10000"), 19, "# PATCH_OFF\t0");
    for (const std::string& text : {no_gate, odd_page, no_patch}) {
        EXPECT_THAT(ParseLayout(text, "sdk.fixups.tsv").status().message(), HasSubstr(kOld)) << text;
    }
}

TEST(LayoutTest, AFarModuleNeedsItsSizes) {
    std::string no_size = Far();
    no_size.erase(no_size.find("# IMAGE_SIZE"), 23);
    std::string odd_len = Far();
    odd_len.replace(odd_len.find("# FILE_IMAGE_LEN\t0xc000"), 23, "# FILE_IMAGE_LEN\t0xc001");
    std::string odd_patch = Far();
    odd_patch.replace(odd_patch.find("# PATCH_OFF\t-0x2000000"), 22, "# PATCH_OFF\t-0x2000100");
    for (const std::string& text : {no_size, odd_len, odd_patch}) {
        EXPECT_THAT(ParseLayout(text, "main.fixups.tsv").status().message(),
                    HasSubstr("patch area before the base, but IMAGE_SIZE / FILE_IMAGE_LEN missing or wrong"))
            << text;
    }
}

TEST(LayoutTest, ABackPatchAreaMustEndTheImage) {
    for (const std::string_view back : {"# BACK_OFF\t0x8000000\n# BACK_LEN\t0x4000\n",  // over the key page
                                        "# BACK_OFF\t0x8004000\n# BACK_LEN\t0x2000\n",  // not whole pages
                                        "# BACK_OFF\t0x8004000\n"}) {                   // no length
        EXPECT_THAT(ParseLayout(Far(back), "main.fixups.tsv").status().message(),
                    HasSubstr("back patch area, but BACK_OFF / BACK_LEN missing or wrong"))
            << back;
    }
    // ... and only a far module has one
    EXPECT_THAT(ParseLayout(Plain("# BACK_OFF\t0x10000\n# BACK_LEN\t0x4000\n"), "main.fixups.tsv").status().message(),
                HasSubstr("back patch area"));
}

TEST(LayoutTest, AtMostEightUnrealConsoleVariables) {
    std::string cvars;
    for (int i = 0; i < 8; ++i) cvars += absl::StrCat("# UE_CVAR:v", i, "\t0x", i, "0\n");
    EXPECT_TRUE(ParseLayout(Plain(cvars), "main.fixups.tsv").ok());
    EXPECT_THAT(ParseLayout(Plain(cvars + "# UE_CVAR:v8\t0x80\n"), "main.fixups.tsv").status().message(),
                HasSubstr("too many Unreal console variables, or too long a name: UE_CVAR:v8"));
    EXPECT_THAT(ParseLayout(Plain(absl::StrCat("# UE_CVAR:", std::string(32, 'n'), "\t0x10\n")), "main.fixups.tsv")
                    .status()
                    .message(),
                HasSubstr("too long a name"));
}

// The tables nso writes for the rewriter's fixtures: a plain module and a far one
absl::StatusOr<ModuleLayout> RewriteAndRead(const std::string& file, const std::string& name) {
    const absl::StatusOr<rewriter::Module> module =
        rewriter::ReadModule(std::filesystem::path(NXMAC_REWRITER_FIXTURES) / file);
    if (!module.ok()) return module.status();
    rewriter::RewriteOptions options;
    options.name = name;
    const absl::StatusOr<rewriter::RewriteResult> result = rewriter::Rewrite(*module, options);
    if (!result.ok()) return result.status();
    return ParseLayout(result->fixups, name + ".fixups.tsv");
}

TEST(LayoutTest, ReadsTheRewritersOwnTables) {
    const absl::StatusOr<ModuleLayout> plain = RewriteAndRead("sys.nso", "sdk");
    ASSERT_TRUE(plain.ok()) << plain.status();
    EXPECT_EQ(plain->patch_off, 0x10000);
    EXPECT_EQ(plain->key_idx, (std::array<unsigned, kKeyCount>{0, 1, 2, 3}));
    EXPECT_EQ(plain->build_id.size(), 32u);
    const absl::StatusOr<ModuleLayout> far = RewriteAndRead("far.nso", "main");
    ASSERT_TRUE(far.ok()) << far.status();
    EXPECT_EQ(far->patch_off, -0x2000000);
    EXPECT_GT(far->image_size, far->file_image_len);
}

TEST(LayoutTest, ReadsTheFileBesideTheImage) {
    const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / "layout_test";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "subsdk0.fixups.tsv") << Plain();
    const absl::StatusOr<ModuleLayout> m = ReadLayout(dir, "subsdk0");
    ASSERT_TRUE(m.ok()) << m.status();
    EXPECT_EQ(m->patch_off, 0x10000);
    const absl::StatusOr<ModuleLayout> missing = ReadLayout(dir, "subsdk1");
    EXPECT_EQ(missing.status().code(), absl::StatusCode::kNotFound);
    EXPECT_THAT(missing.status().message(), HasSubstr("subsdk1.fixups.tsv"));
}

}  // namespace
}  // namespace nxmac::loader
