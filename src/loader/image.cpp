#include "nxmac/loader/image.h"

#include <sys/mman.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace nxmac::loader {
namespace {

constexpr std::uint64_t RoundUp(std::uint64_t v) { return (v + kPageSize - 1) & ~(kPageSize - 1); }

// The frame step state the rewriter's stubs read and the host writes: at least this much of the page is used
constexpr std::uint64_t kFpsStateUsed = 0x40;

absl::Status Protect(std::uint8_t* at, std::uint64_t len, int prot, std::string_view name, std::string_view what) {
    if (mprotect(at, len, prot) != 0) return absl::InternalError(absl::StrCat(name, " mprotect ", what));
    return absl::OkStatus();
}

}  // namespace

ModuleImage::ModuleImage(ModuleImage&& other) noexcept { *this = std::move(other); }

ModuleImage& ModuleImage::operator=(ModuleImage&& other) noexcept {
    if (this != &other) {
        if (map_ != nullptr) munmap(map_, map_len_);
        map_ = std::exchange(other.map_, nullptr);
        map_len_ = std::exchange(other.map_len_, 0);
        base_ = std::exchange(other.base_, nullptr);
        size_ = std::exchange(other.size_, 0);
        patch_len_ = std::exchange(other.patch_len_, 0);
        fps_state_ = std::exchange(other.fps_state_, nullptr);
    }
    return *this;
}

ModuleImage::~ModuleImage() {
    if (map_ != nullptr) munmap(map_, map_len_);
}

absl::StatusOr<ModuleImage> MapModule(const std::filesystem::path& dir, std::string_view name,
                                      const ModuleLayout& layout, const HostKeys& keys) {
    const std::filesystem::path path = dir / absl::StrCat(name, ".patched.bin");
    std::ifstream in(path, std::ios::binary);
    if (!in) return absl::NotFoundError(absl::StrCat("open ", path.string()));
    const std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return absl::InternalError(absl::StrCat("read ", path.string()));
    const std::uint64_t sz = raw.size();

    ModuleImage m;
    const auto map = [&](std::uint64_t len) -> absl::Status {
        void* const a = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (a == MAP_FAILED) return absl::InternalError(absl::StrFormat("%s mmap %#x", name, len));
        m.map_ = a;
        m.map_len_ = len;
        return absl::OkStatus();
    };
    // install_key_page: the four keys' slot offsets, then the page read-only; install_fps_state: the page after it
    const auto install_key_page = [&]() -> absl::Status {
        if (layout.key_page_off + layout.key_page_size > m.size_) {
            return absl::InvalidArgumentError(absl::StrCat(name, ": key page out of range"));
        }
        auto* const page = reinterpret_cast<std::uint64_t*>(m.base_ + layout.key_page_off);
        std::memset(page, 0, layout.key_page_size);
        for (int k = 0; k < kKeyCount; ++k) {
            if (layout.key_idx[k] >= layout.key_page_size / 8) {
                return absl::InvalidArgumentError(absl::StrFormat(
                    "%s: key page entry %d at index %u is outside the page", name, k, layout.key_idx[k]));
            }
            page[layout.key_idx[k]] = keys[k] * 8;
        }
        for (int k = 0; k < kKeyCount; ++k) {
            if (page[layout.key_idx[k]] == 0) {
                return absl::InvalidArgumentError(absl::StrFormat(
                    "%s: key page entry %d is 0 (host key not created) -- the game would read TSD slot 0", name, k));
            }
        }
        if (mprotect(m.base_ + layout.key_page_off, layout.key_page_size, PROT_READ) != 0) {
            return absl::InternalError(absl::StrCat(name, " key page mprotect"));
        }
        if (layout.fps_state_off != 0) {
            if (layout.fps_state_off + kFpsStateUsed > m.size_ ||
                layout.fps_state_off < layout.key_page_off + layout.key_page_size) {
                return absl::InvalidArgumentError(absl::StrCat(name, ": frame step state page out of range"));
            }
            m.fps_state_ = m.base_ + layout.fps_state_off;
        }
        return absl::OkStatus();
    };
    const std::uint64_t rx = RoundUp(layout.text_end);

    if (layout.patch_off < 0) {
        // The patch area before the base: the file is the image's first part (text + ro + data), then the patch area;
        // bss and the key page are zero pages. The gap from the patch area's end to the base is left inaccessible. With
        // a back patch area (TWO) the file ends with it, BACK_LEN bytes, copied to BACK_OFF in the image.
        const std::uint64_t pre = 0 - static_cast<std::uint64_t>(layout.patch_off);
        const std::uint64_t fl = layout.file_image_len;
        const std::uint64_t bl = layout.back_len;
        if (sz < fl + bl) {
            return absl::InvalidArgumentError(
                absl::StrFormat("%s is shorter than FILE_IMAGE_LEN %#x + BACK_LEN %#x", path.string(), fl, bl));
        }
        if (fl > layout.image_size) {
            return absl::InvalidArgumentError(absl::StrFormat("%s: FILE_IMAGE_LEN %#x is larger than IMAGE_SIZE %#x",
                                                              path.string(), fl, layout.image_size));
        }
        m.size_ = layout.image_size;
        m.patch_len_ = RoundUp(sz - bl - fl);
        if (m.patch_len_ > pre) {
            return absl::InvalidArgumentError(
                absl::StrFormat("%s patch area of %#x bytes does not fit in the %#x before the base", path.string(),
                                m.patch_len_, pre));
        }
        if (absl::Status s = map(pre + m.size_); !s.ok()) return s;
        auto* const a = static_cast<std::uint8_t*>(m.map_);
        m.base_ = a + pre;
        std::memcpy(m.base_, raw.data(), fl);
        std::memcpy(a, raw.data() + fl, sz - bl - fl);
        if (bl != 0) std::memcpy(m.base_ + layout.back_off, raw.data() + (sz - bl), bl);
        if (absl::Status s = install_key_page(); !s.ok()) return s;
        if (bl != 0) {
            if (absl::Status s = Protect(m.base_ + layout.back_off, bl, PROT_READ | PROT_EXEC, name, "back patch");
                !s.ok()) {
                return s;
            }
        }
        if (absl::Status s = Protect(m.base_, rx, PROT_READ | PROT_EXEC, name, "text"); !s.ok()) return s;
        if (absl::Status s = Protect(a, m.patch_len_, PROT_READ | PROT_EXEC, name, "patch"); !s.ok()) return s;
        if (pre > m.patch_len_) {
            if (absl::Status s = Protect(a + m.patch_len_, pre - m.patch_len_, PROT_NONE, name, "gap"); !s.ok())
                return s;
        }
        return m;
    }

    m.size_ = RoundUp(sz);
    if (absl::Status s = map(m.size_); !s.ok()) return s;
    m.base_ = static_cast<std::uint8_t*>(m.map_);
    std::memcpy(m.base_, raw.data(), sz);
    if (absl::Status s = install_key_page(); !s.ok()) return s;
    if (absl::Status s = Protect(m.base_, rx, PROT_READ | PROT_EXEC, name, "text"); !s.ok()) return s;
    const std::uint64_t po = static_cast<std::uint64_t>(layout.patch_off) & ~(kPageSize - 1);
    if (po >= m.size_) {
        return absl::InvalidArgumentError(
            absl::StrFormat("%s: PATCH_OFF %#x is past the image's %#x bytes", path.string(), po, m.size_));
    }
    if (absl::Status s = Protect(m.base_ + po, m.size_ - po, PROT_READ | PROT_EXEC, name, "patch"); !s.ok()) return s;
    return m;
}

}  // namespace nxmac::loader
