#include "nxmac/loader/layout.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace nxmac::loader {
namespace {

// isspace in the C locale: what sscanf skips and %s stops at
bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

std::string_view SkipSpace(std::string_view s) {
    while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
    return s;
}

// The next run of non-space characters, at most `width` of them (sscanf's %<width>s); `s` is left after it
std::string_view TakeWord(std::string_view& s, std::size_t width) {
    std::size_t n = 0;
    while (n < s.size() && n < width && !IsSpace(s[n])) ++n;
    const std::string_view word = s.substr(0, n);
    s.remove_prefix(n);
    return word;
}

// A number at the start of `s` as strtoull / strtoll read it (leading space, a sign, 0x in base 16; out of range is the
// largest value), or nullopt when no digit converts: the C library on a copy, so the reading is the C library's
std::optional<std::uint64_t> ReadUnsigned(std::string_view s, int base) {
    const std::string c(s);
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(c.c_str(), &end, base);
    if (end == c.c_str()) return std::nullopt;
    return static_cast<std::uint64_t>(v);
}

std::int64_t ReadSigned(std::string_view s, int base) {
    const std::string c(s);
    errno = 0;
    return static_cast<std::int64_t>(std::strtoll(c.c_str(), nullptr, base));
}

constexpr std::array<std::string_view, kKeyCount> kKeyNames = {"K18", "KTLS", "KTLR", "KGATE"};

}  // namespace

absl::StatusOr<ModuleLayout> ParseLayout(std::string_view text, std::string_view path) {
    const auto old_rewriter = [&] {
        return absl::InvalidArgumentError(
            absl::StrCat(path, ": the output comes from an old rewriter; rewrite the modules again with nso"));
    };
    ModuleLayout m;
    unsigned seen = 0;
    while (!text.empty()) {
        // A line with its newline, as fgets gives it
        const std::size_t nl = text.find('\n');
        const std::string_view line = text.substr(0, nl == std::string_view::npos ? text.size() : nl + 1);
        text.remove_prefix(line.size());

        // sscanf(line, "# %47s %lx", key, &v) == 2
        if (line.empty() || line.front() != '#') return old_rewriter();
        std::string_view rest = SkipSpace(line.substr(1));
        const std::string_view key = TakeWord(rest, 47);
        if (key.empty()) return old_rewriter();
        const std::optional<std::uint64_t> value = ReadUnsigned(rest, 16);
        if (!value) return old_rewriter();
        const std::uint64_t v = *value;

        if (key == "PATCH_OFF") {
            m.patch_off = ReadSigned(rest, 16);  // may be negative (the patch area before the base)
        } else if (key == "IMAGE_SIZE") {
            m.image_size = v;
        } else if (key == "FILE_IMAGE_LEN") {
            m.file_image_len = v;
        } else if (key == "BACK_OFF") {
            m.back_off = v;
        } else if (key == "BACK_LEN") {
            m.back_len = v;
        } else if (key == "KEY_PAGE_OFF") {
            m.key_page_off = v;
        } else if (key == "KEY_PAGE_SIZE") {
            m.key_page_size = v;
        } else if (key == "TEXT_END") {
            m.text_end = v;
        } else if (key == "RO_OFF") {
            m.ro_off = v;
        } else if (key == "DATA_OFF") {
            m.data_off = v;
        } else if (key == "FPS_STATE_OFF") {
            m.fps_state_off = v;
        } else if (key == "RENDER_SCALE") {
            m.render_scale_max = static_cast<std::uint32_t>(v);
        } else if (key == "UE_MALLOC_VT") {
            m.ue_malloc_vt = v;
        } else if (key == "UE_MALLOC_SLOT") {
            m.ue_malloc_slot = static_cast<unsigned>(v);
        } else if (key == "FPS_TARGET_PTR") {
            m.fps_target_ptr = v;
        } else if (key == "FPS_TARGET_FIELD") {
            m.fps_target_field = static_cast<std::uint32_t>(v);
        } else if (key == "UE_FIXED_RATE_PTR") {
            m.ue_fixed_ptr = v;
        } else if (key == "UE_FIXED_RATE_FIELD") {
            m.ue_fixed_field = static_cast<std::uint32_t>(v);
        } else if (key == "UE_FIXED_RATE_FLAG") {
            m.ue_fixed_flag = static_cast<std::uint32_t>(v);
        } else if (key == "UNITY_TARGET_FPS") {
            m.unity_target_fps = v;
        } else if (key == "UNITY_QS_PTR") {
            m.unity_qs_ptr = v;
        } else if (key == "UNITY_QS_IDX") {
            m.unity_qs_idx = static_cast<std::uint32_t>(v);
        } else if (key == "UNITY_QS_ARR") {
            m.unity_qs_arr = static_cast<std::uint32_t>(v);
        } else if (key == "UNITY_QS_SIZE") {
            m.unity_qs_size = static_cast<std::uint32_t>(v);
        } else if (key == "UNITY_QS_VSYNC") {
            m.unity_qs_vsync = static_cast<std::uint32_t>(v);
        } else if (key.starts_with("UE_CVAR:")) {
            const std::string_view name = key.substr(8);
            if (m.ue_cvars.size() == kMaxUeCvars || name.size() > kMaxUeCvarName) {
                return absl::InvalidArgumentError(
                    absl::StrFormat("%s: too many Unreal console variables, or too long a name: %s", path, key));
            }
            m.ue_cvars.push_back({std::string(name), v});
        } else if (key == "BUILD_ID") {
            // sscanf(line, "# %*s %32s", build_id): the second word, at most 32 characters
            std::string_view words = SkipSpace(line.substr(1));
            TakeWord(words, std::string_view::npos);
            words = SkipSpace(words);
            m.build_id = std::string(TakeWord(words, 32));
        } else {
            for (int k = 0; k < kKeyCount; ++k) {
                if (key == absl::StrCat("KEY_", kKeyNames[k], "_IDX")) {
                    // The decimal number after the line's last tab
                    const std::size_t tab = line.rfind('\t');
                    const std::optional<std::uint64_t> idx =
                        ReadUnsigned(tab == std::string_view::npos ? rest : line.substr(tab + 1), 10);
                    m.key_idx[k] = static_cast<unsigned>(idx.value_or(0));
                    seen |= 1u << k;
                }
            }
        }
        // Any other key (main's capabilities, for one) is not the loader's
    }

    if (m.patch_off == 0 || m.key_page_off % kPageSize != 0 || m.key_page_size != kPageSize ||
        seen != (1u << kKeyCount) - 1) {
        return old_rewriter();
    }
    if (m.patch_off < 0 &&
        (m.image_size == 0 || m.file_image_len == 0 || m.file_image_len % kPageSize != 0 ||
         m.patch_off % static_cast<std::int64_t>(kPageSize) != 0 || m.key_page_off + m.key_page_size > m.image_size)) {
        return absl::InvalidArgumentError(
            absl::StrCat(path, ": patch area before the base, but IMAGE_SIZE / FILE_IMAGE_LEN missing or wrong"));
    }
    if ((m.back_off != 0 || m.back_len != 0) &&
        (m.patch_off >= 0 || m.back_len == 0 || m.back_off % kPageSize != 0 || m.back_len % kPageSize != 0 ||
         m.back_off < m.key_page_off + m.key_page_size || m.back_off + m.back_len != m.image_size)) {
        return absl::InvalidArgumentError(
            absl::StrCat(path, ": back patch area, but BACK_OFF / BACK_LEN missing or wrong"));
    }
    return m;
}

absl::StatusOr<ModuleLayout> ReadLayout(const std::filesystem::path& dir, std::string_view module) {
    const std::filesystem::path path = dir / absl::StrCat(module, ".fixups.tsv");
    std::ifstream in(path, std::ios::binary);
    if (!in) return absl::NotFoundError(absl::StrCat("open ", path.string()));
    std::ostringstream text;
    text << in.rdbuf();
    return ParseLayout(text.str(), path.string());
}

}  // namespace nxmac::loader
