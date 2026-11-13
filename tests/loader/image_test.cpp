#include "nxmac/loader/image.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace nxmac::loader {
namespace {

using ::testing::HasSubstr;

constexpr HostKeys kKeys = {11, 12, 13, 14};

// A module's file: `n` bytes of a pattern that differs from page to page
std::vector<std::uint8_t> Bytes(std::size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>(seed + i * 7 + i / 0x4000);
    return b;
}

std::filesystem::path Folder(const std::string& test) {
    const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / "image_test" / test;
    std::filesystem::create_directories(dir);
    return dir;
}

void Write(const std::filesystem::path& dir, const std::string& name, const std::vector<std::uint8_t>& bytes) {
    std::ofstream(dir / (name + ".patched.bin"), std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// What the pages allow, tried the hard way (a write to a read-only page, a read of an inaccessible one, ends the
// process)
void Poke(std::uint8_t* p) { *static_cast<volatile std::uint8_t*>(p) = 0x5a; }
std::uint8_t Peek(const std::uint8_t* p) { return *static_cast<const volatile std::uint8_t*>(p); }

bool Same(const std::uint8_t* at, const std::uint8_t* want, std::size_t n) { return std::memcmp(at, want, n) == 0; }
bool Zero(const std::uint8_t* at, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (at[i] != 0) return false;
    return true;
}

// A plain module: text to 0x1000 (one page read + execute), ro + data to 0x8000, the key page, the frame step state
// page, the patch area at 0x10000 (one page of the file's, then zeros to the image's end)
ModuleLayout Plain() {
    ModuleLayout l;
    l.patch_off = 0x10000;
    l.text_end = 0x1000;
    l.ro_off = 0x4000;
    l.data_off = 0x6000;
    l.key_page_off = 0x8000;
    l.key_page_size = kPageSize;
    l.key_idx = {0, 1, 2, 3};
    return l;
}

TEST(MapModuleTest, APlainModule) {
    const std::filesystem::path dir = Folder("plain");
    const std::vector<std::uint8_t> file = Bytes(0x11000, 1);
    Write(dir, "sdk", file);
    ModuleLayout l = Plain();
    l.fps_state_off = 0xc000;
    l.key_idx = {3, 0, 2, 1};
    absl::StatusOr<ModuleImage> m = MapModule(dir, "sdk", l, kKeys);
    ASSERT_TRUE(m.ok()) << m.status();
    EXPECT_EQ(m->size(), 0x14000u);
    EXPECT_EQ(m->patch_len(), 0u);
    EXPECT_EQ(m->fps_state(), m->base() + 0xc000);
    std::uint8_t* const b = m->base();
    EXPECT_TRUE(Same(b, file.data(), 0x8000));  // the file up to the key page
    // The key page: each key's slot offset at its index, nothing else
    const auto* page = reinterpret_cast<const std::uint64_t*>(b + 0x8000);
    EXPECT_EQ(page[3], 11u * 8);
    EXPECT_EQ(page[0], 12u * 8);
    EXPECT_EQ(page[2], 13u * 8);
    EXPECT_EQ(page[1], 14u * 8);
    EXPECT_TRUE(Zero(b + 0x8020, kPageSize - 0x20));
    EXPECT_TRUE(
        Same(b + 0xc000, file.data() + 0xc000, 0x5000));  // the state page and the patch area as the file has them
    EXPECT_TRUE(Zero(b + 0x11000, 0x3000));               // the rest of the patch area's last page

    Poke(b + 0x4000);  // ro + data: writable
    Poke(b + 0x7fff);
    Poke(m->fps_state());
    EXPECT_EQ(Peek(b + 0x7fff), 0x5a);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(Poke(b), "");            // the text
    EXPECT_DEATH(Poke(b + 0x3fff), "");   // ... to the end of TEXT_END's page
    EXPECT_DEATH(Poke(b + 0x8000), "");   // the key page
    EXPECT_DEATH(Poke(b + 0x10000), "");  // the patch area
    EXPECT_DEATH(Poke(b + 0x13fff), "");
}

// A far module: 0x8000 bytes of image in the file (text, ro, data), bss to the key page, the key page at 0xc000; the
// patch area (0x2000 bytes in the file, one page mapped) 0x8000 before the base
ModuleLayout Far() {
    ModuleLayout l = Plain();
    l.patch_off = -0x8000;
    l.image_size = 0x10000;
    l.file_image_len = 0x8000;
    l.key_page_off = 0xc000;
    return l;
}

TEST(MapModuleTest, AFarModuleHasItsPatchAreaBeforeTheBase) {
    const std::filesystem::path dir = Folder("far");
    const std::vector<std::uint8_t> file = Bytes(0xa000, 2);
    Write(dir, "main", file);
    absl::StatusOr<ModuleImage> m = MapModule(dir, "main", Far(), kKeys);
    ASSERT_TRUE(m.ok()) << m.status();
    EXPECT_EQ(m->size(), 0x10000u);
    EXPECT_EQ(m->patch_len(), 0x4000u);
    EXPECT_EQ(m->fps_state(), nullptr);
    std::uint8_t* const b = m->base();
    std::uint8_t* const front = b - 0x8000;
    EXPECT_TRUE(Same(b, file.data(), 0x8000));
    EXPECT_TRUE(Zero(b + 0x8000, 0x4000));                   // bss
    EXPECT_TRUE(Same(front, file.data() + 0x8000, 0x2000));  // the patch area
    EXPECT_TRUE(Zero(front + 0x2000, 0x2000));
    EXPECT_EQ(reinterpret_cast<const std::uint64_t*>(b + 0xc000)[3], 14u * 8);

    Poke(b + 0x8000);  // bss: writable
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(Poke(front), "");           // the patch area
    EXPECT_DEATH(Peek(front + 0x4000), "");  // the gap to the base: not even readable
    EXPECT_DEATH(Peek(b - 1), "");
    EXPECT_DEATH(Poke(b), "");           // the text
    EXPECT_DEATH(Poke(b + 0xc000), "");  // the key page
}

TEST(MapModuleTest, AFarModulesBackPatchAreaEndsTheImage) {
    const std::filesystem::path dir = Folder("back");
    const std::vector<std::uint8_t> file = Bytes(0xe000, 3);  // image 0x8000, front 0x2000, back 0x4000
    Write(dir, "main", file);
    ModuleLayout l = Far();
    l.image_size = 0x14000;
    l.back_off = 0x10000;
    l.back_len = 0x4000;
    absl::StatusOr<ModuleImage> m = MapModule(dir, "main", l, kKeys);
    ASSERT_TRUE(m.ok()) << m.status();
    EXPECT_EQ(m->size(), 0x14000u);
    EXPECT_EQ(m->patch_len(), 0x4000u);
    EXPECT_TRUE(Same(m->base() - 0x8000, file.data() + 0x8000, 0x2000));
    EXPECT_TRUE(Same(m->base() + 0x10000, file.data() + 0xa000, 0x4000));
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(Poke(m->base() + 0x10000), "");
}

TEST(MapModuleTest, WhatStops) {
    const std::filesystem::path dir = Folder("stops");
    const auto map = [&](const ModuleLayout& l, std::size_t file, HostKeys keys = kKeys) {
        Write(dir, "m", Bytes(file, 4));
        return MapModule(dir, "m", l, keys).status();
    };
    const absl::Status missing = MapModule(dir, "none", Plain(), kKeys).status();
    EXPECT_EQ(missing.code(), absl::StatusCode::kNotFound);
    EXPECT_THAT(missing.message(), HasSubstr("open "));
    EXPECT_THAT(missing.message(), HasSubstr("none.patched.bin"));

    EXPECT_THAT(map(Far(), 0x7000).message(), HasSubstr("is shorter than FILE_IMAGE_LEN 0x8000 + BACK_LEN 0"));
    EXPECT_THAT(map(Far(), 0x8000 + 0x9000).message(),
                HasSubstr("patch area of 0xc000 bytes does not fit in the 0x8000 before the base"));
    EXPECT_THAT(map(Plain(), 0x8000).message(), HasSubstr("m: key page out of range"));
    EXPECT_THAT(map(Plain(), 0x9000).message(), HasSubstr("PATCH_OFF 0x10000 is past the image's 0xc000 bytes"));
    EXPECT_THAT(map(Plain(), 0x11000, {11, 0, 13, 14}).message(),
                HasSubstr("m: key page entry 1 is 0 (host key not created) -- the game would read TSD slot 0"));
    ModuleLayout state = Plain();
    state.fps_state_off = 0x8000;  // on the key page
    EXPECT_THAT(map(state, 0x11000).message(), HasSubstr("m: frame step state page out of range"));
    state.fps_state_off = 0x13fe0;  // too near the end
    EXPECT_THAT(map(state, 0x11000).message(), HasSubstr("m: frame step state page out of range"));
}

}  // namespace
}  // namespace nxmac::loader
