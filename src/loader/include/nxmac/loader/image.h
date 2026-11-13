// A rewritten module's image mapped into the host, where its code runs: <module>.patched.bin copied into anonymous
// pages at the layout its table gives, the key page filled with the host's thread keys, the code sealed read + execute
// before anything else runs. A port of the Mac path of nxnative's dm/host/loader.c map_image, install_key_page and
// install_fps_state. Not ported yet: the layout of a served JIT image (DM_JIT_SERVE, jit_slot), which places modules at
// fixed distances; here the system picks each module's address.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

#include "absl/status/statusor.h"
#include "nxmac/loader/layout.h"

namespace nxmac::loader {

// The host's thread keys in key_idx's order (K18, KTLS, KTLR, KGATE): the key page holds each one times 8, the offset
// of its slot from the thread's TSD base, which the rewritten code reads through tpidrro_el0
using HostKeys = std::array<std::uint64_t, kKeyCount>;

// The mapping, unmapped when destroyed (nxhost keeps its modules for the process's life). Its pages: the code (up to
// TEXT_END) and the patch areas read + execute, the key page read-only, a far module's gap between its patch area and
// its base inaccessible, the rest read + write.
class ModuleImage {
public:
    ModuleImage() = default;
    ModuleImage(ModuleImage&& other) noexcept;
    ModuleImage& operator=(ModuleImage&& other) noexcept;
    ModuleImage(const ModuleImage&) = delete;
    ModuleImage& operator=(const ModuleImage&) = delete;
    ~ModuleImage();

    std::uint8_t* base() const { return base_; }  // the image's start (a far module's patch area is before it)
    std::size_t size() const { return size_; }    // module_t's size: a far module's whole image, without its front
    std::uint64_t patch_len() const { return patch_len_; }  // a far module's front patch area, whole pages; 0 otherwise
    std::uint8_t* fps_state() const { return fps_state_; }  // the frame step state page; null when the module has none

private:
    friend absl::StatusOr<ModuleImage> MapModule(const std::filesystem::path&, std::string_view, const ModuleLayout&,
                                                 const HostKeys&);
    void* map_ = nullptr;  // the whole mapping, a far module's front included
    std::size_t map_len_ = 0;
    std::uint8_t* base_ = nullptr;
    std::size_t size_ = 0;
    std::uint64_t patch_len_ = 0;
    std::uint8_t* fps_state_ = nullptr;
};

// <dir>/<name>.patched.bin mapped as `layout` says. NotFound when the file cannot be opened; InvalidArgument (where
// nxnative's host stops) when it does not fit the layout, its key page or frame step state page is out of range, or a
// key is 0; Internal when the system refuses a mapping or a protection.
absl::StatusOr<ModuleImage> MapModule(const std::filesystem::path& dir, std::string_view name,
                                      const ModuleLayout& layout, const HostKeys& keys);

}  // namespace nxmac::loader
