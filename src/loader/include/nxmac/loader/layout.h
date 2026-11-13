// A rewritten module's layout: the "# KEY value" lines of <module>.fixups.tsv, which nso (and nxnative's patch_nso.py)
// writes beside <module>.patched.bin. The loader maps the image by it. A port of nxnative's dm/host/loader.c
// read_layout, with module_t's fields that it fills (dm/host/dm.h).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace nxmac::loader {

// The host's page (16 KiB): the key page, the patch areas and a far module's file image are whole pages
inline constexpr std::uint64_t kPageSize = 0x4000;

// The thread slots the rewriter gives a key page entry each, in key_idx's order (KEY_<name>_IDX in the table)
enum Key : int { kKeyK18, kKeyTls, kKeyTlr, kKeyGate, kKeyCount };

// An Unreal console variable the rewriter found by its registered name (UE_CVAR:<name>): its TAutoConsoleVariable
struct UeCvar {
    std::string name;
    std::uint64_t off = 0;
};
inline constexpr std::size_t kMaxUeCvars = 8;      // module_t's ue_cvar[8]
inline constexpr std::size_t kMaxUeCvarName = 31;  // its char name[32]

struct ModuleLayout {
    // The patch area relative to the base; negative: before the base (a module too large for a branch to reach past
    // it, patch_nso.py FAR), and image_size / file_image_len say what the file holds
    std::int64_t patch_off = 0;
    std::uint64_t image_size = 0;      // FAR: the whole image, bss and key page included
    std::uint64_t file_image_len = 0;  // FAR: the image's part of the file
    std::uint64_t back_off = 0;        // a second patch area at the image's end (patch_nso.py TWO); 0 = none
    std::uint64_t back_len = 0;
    std::uint64_t key_page_off = 0;
    std::uint64_t key_page_size = 0;
    std::uint64_t text_end = 0;
    std::uint64_t ro_off = 0;
    std::uint64_t data_off = 0;
    std::uint64_t fps_state_off = 0;     // the frame step state page; 0 = none
    std::uint32_t render_scale_max = 0;  // a render-scale stub's largest scale; 0 = no such stub
    std::uint64_t ue_malloc_vt = 0;      // Unreal's FMallocBinned3 vtable; 0 = none
    unsigned ue_malloc_slot = 0;
    std::vector<UeCvar> ue_cvars;
    std::uint64_t fps_target_ptr = 0;  // a target frame rate at [main + fps_target_ptr] + fps_target_field; 0 = none
    std::uint32_t fps_target_field = 0;
    std::uint64_t ue_fixed_ptr = 0;  // Unreal's GEngine and UEngine::FixedFrameRate in it; 0 = none
    std::uint32_t ue_fixed_field = 0;
    std::uint32_t ue_fixed_flag = 0;
    std::uint64_t unity_target_fps = 0;  // Unity's Application.targetFrameRate; 0 = none
    std::uint64_t unity_qs_ptr = 0;      // Unity's QualitySettings and the current level's vSyncCount in it; 0 = none
    std::uint32_t unity_qs_idx = 0;
    std::uint32_t unity_qs_arr = 0;
    std::uint32_t unity_qs_size = 0;
    std::uint32_t unity_qs_vsync = 0;
    std::string build_id;  // the first 16 bytes of the NSO build ID, in hex; empty from older rewriters
    std::array<unsigned, kKeyCount> key_idx{};  // each thread slot's key page index
};

// A layout table's text; `path` names it in the errors. InvalidArgument (where nxnative's host stops) when a line is
// not "# KEY value" or the table lacks what the loader needs (the output of an old rewriter), and when a far module's
// or a back patch area's values do not fit together.
absl::StatusOr<ModuleLayout> ParseLayout(std::string_view text, std::string_view path);

// <dir>/<module>.fixups.tsv. NotFound when it cannot be opened.
absl::StatusOr<ModuleLayout> ReadLayout(const std::filesystem::path& dir, std::string_view module);

}  // namespace nxmac::loader
