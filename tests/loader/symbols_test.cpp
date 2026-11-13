#include "nxmac/loader/symbols.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nxmac/loader/image.h"
#include "nxmac/loader/layout.h"
#include "nxmac/rewriter/module.h"
#include "nxmac/rewriter/rewrite.h"

namespace nxmac::loader {
namespace {

using ::testing::HasSubstr;

template <typename T>
void Put(std::vector<std::uint8_t>& img, std::size_t off, const T& v) {
    std::memcpy(img.data() + off, &v, sizeof v);
}

struct Sym {
    std::string name;
    std::uint16_t shndx;  // 0: undefined
    std::uint64_t value;
};

// A made-up image: MOD0 at 0x100, the dynamic section at 0x200, the symbols at 0x400, the strings after them
std::vector<std::uint8_t> Image(const std::vector<Sym>& syms) {
    std::vector<std::uint8_t> img(0x400 + (syms.size() + 1) * sizeof(Elf64Sym) + 0x10000);
    Put<std::uint32_t>(img, 4, 0x100);
    std::memcpy(img.data() + 0x100, "MOD0", 4);
    Put<std::int32_t>(img, 0x104, 0x100);
    const std::uint64_t strtab = 0x400 + (syms.size() + 1) * sizeof(Elf64Sym);
    const Elf64Dyn dyn[] = {{kDtSymtab, 0x400},     {kDtStrtab, strtab},    {kDtInit, 0x1000}, {kDtFini, 0x1100},
                            {kDtInitArray, 0x1200}, {kDtInitArraySz, 0x18}, {10, 0x55},        {0, 0}};
    std::memcpy(img.data() + 0x200, dyn, sizeof dyn);
    std::size_t str = 1;  // the empty name at 0
    for (std::size_t i = 0; i < syms.size(); ++i) {
        const Elf64Sym s = {static_cast<std::uint32_t>(str), 0x12, 0, syms[i].shndx, syms[i].value, 0};
        Put(img, 0x400 + (i + 1) * sizeof(Elf64Sym), s);
        std::memcpy(img.data() + strtab + str, syms[i].name.c_str(), syms[i].name.size() + 1);
        str += syms[i].name.size() + 1;
    }
    return img;
}

TEST(ModuleSymbolsTest, ReadsTheDynamicSection) {
    std::vector<std::uint8_t> img =
        Image({{"foo", 1, 0x10}, {"bar", 0, 0}, {"foo", 1, 0x20}, {"baz", 7, 0x30}, {"nnMain", 1, 0x4000}});
    const absl::StatusOr<ModuleSymbols> s = ModuleSymbols::Parse(img.data(), img.size(), "main");
    ASSERT_TRUE(s.ok()) << s.status();
    EXPECT_EQ(s->nsym(), 6u);
    EXPECT_EQ(s->init(), 0x1000u);
    EXPECT_EQ(s->fini(), 0x1100u);
    EXPECT_EQ(s->init_array(), 0x1200u);
    EXPECT_EQ(s->init_arraysz(), 0x18u);
    EXPECT_EQ(s->hash_slots(), 16u);
    EXPECT_EQ(s->Find("foo"), img.data() + 0x10);  // the first of two
    EXPECT_EQ(s->Find("bar"), nullptr);            // an import
    EXPECT_EQ(s->Find("baz"), img.data() + 0x30);
    EXPECT_EQ(s->Find("nnMain"), img.data() + 0x4000);
    EXPECT_EQ(s->Find("nnMai"), nullptr);
    EXPECT_EQ(s->Find("nnMainx"), nullptr);
    EXPECT_EQ(s->Find(""), nullptr);
    for (const char* n : {"foo", "bar", "baz", "nnMain", "nnMai", "none"}) EXPECT_EQ(s->Find(n), s->FindLinear(n)) << n;
}

TEST(ModuleSymbolsTest, TheHashGivesWhatTheScanGives) {
    // Enough names for the table to wrap and collide, defined twice at different values, and some only imported
    std::vector<Sym> syms;
    syms.reserve(4000);
    for (int i = 0; i < 3000; ++i)
        syms.push_back(
            {absl::StrCat("_ZN2nn2os", i), static_cast<std::uint16_t>(i % 5), static_cast<std::uint64_t>(i)});
    for (int i = 0; i < 3000; i += 3) syms.push_back({absl::StrCat("_ZN2nn2os", i), 1, 0x100000u + i});
    std::vector<std::uint8_t> img = Image(syms);
    const absl::StatusOr<ModuleSymbols> s = ModuleSymbols::Parse(img.data(), img.size(), "sdk");
    ASSERT_TRUE(s.ok()) << s.status();
    EXPECT_EQ(s->hash_slots(), 8192u);
    for (int i = 0; i < 3100; ++i) {
        const std::string n = absl::StrCat("_ZN2nn2os", i);
        ASSERT_EQ(s->Find(n), s->FindLinear(n)) << n;
    }
    EXPECT_EQ(s->Find("_ZN2nn2os5"), nullptr);                      // 5 % 5 == 0: only imported
    EXPECT_EQ(s->Find("_ZN2nn2os15"), img.data() + 0x100000 + 15);  // imported first, defined by its second entry
    EXPECT_EQ(s->Find("_ZN2nn2os6"), img.data() + 6);               // defined twice: the first one
    EXPECT_EQ(s->Find("_ZN2nn2os7"), img.data() + 7);
}

TEST(ModuleSymbolsTest, WhatStops) {
    std::vector<std::uint8_t> img = Image({{"foo", 1, 0x10}});
    std::vector<std::uint8_t> no_mod0 = img;
    no_mod0[0x100] = 'X';
    EXPECT_THAT(ModuleSymbols::Parse(no_mod0.data(), no_mod0.size(), "sdk").status().message(),
                HasSubstr("sdk: no MOD0"));
    std::vector<std::uint8_t> far_mod0 = img;
    Put<std::uint32_t>(far_mod0, 4, 0x7ffffff0);
    EXPECT_THAT(ModuleSymbols::Parse(far_mod0.data(), far_mod0.size(), "sdk").status().message(),
                HasSubstr("sdk: no MOD0"));
    std::vector<std::uint8_t> far_dyn = img;
    Put<std::int32_t>(far_dyn, 0x104, 0x7ffffff0);
    EXPECT_THAT(ModuleSymbols::Parse(far_dyn.data(), far_dyn.size(), "sdk").status().message(),
                HasSubstr("sdk: the dynamic section is outside the image"));
    std::vector<std::uint8_t> no_symtab = img;
    Put<std::int64_t>(no_symtab, 0x200, 99);  // DT_SYMTAB becomes another tag
    EXPECT_THAT(ModuleSymbols::Parse(no_symtab.data(), no_symtab.size(), "sdk").status().message(),
                HasSubstr("sdk: the symbol table is outside the image"));
    std::vector<std::uint8_t> long_name = img;
    Put<std::uint32_t>(long_name, 0x400 + sizeof(Elf64Sym), 0x7fffffff);
    EXPECT_THAT(ModuleSymbols::Parse(long_name.data(), long_name.size(), "sdk").status().message(),
                HasSubstr("symbol 1's name is outside the image"));
}

// The rewriter's fixture modules, written as nso writes them, mapped, and read
TEST(ModuleSymbolsTest, ReadsARewrittenModule) {
    for (const char* file : {"sys.nso", "far.nso", "data.nso"}) {
        const absl::StatusOr<rewriter::Module> module =
            rewriter::ReadModule((std::filesystem::path(NXMAC_REWRITER_FIXTURES) / file).string());
        ASSERT_TRUE(module.ok()) << module.status();
        rewriter::RewriteOptions options;
        options.name = "main";
        const absl::StatusOr<rewriter::RewriteResult> result = rewriter::Rewrite(*module, options);
        ASSERT_TRUE(result.ok()) << result.status();
        const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / "symbols_test" / file;
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "main.patched.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(result->patched.data()),
                   static_cast<std::streamsize>(result->patched.size()));
        const absl::StatusOr<ModuleLayout> layout = ParseLayout(result->fixups, "main.fixups.tsv");
        ASSERT_TRUE(layout.ok()) << layout.status();
        absl::StatusOr<ModuleImage> image = MapModule(dir, "main", *layout, {11, 12, 13, 14});
        ASSERT_TRUE(image.ok()) << image.status();
        const absl::StatusOr<ModuleSymbols> s = ModuleSymbols::Parse(image->base(), image->size(), "main");
        ASSERT_TRUE(s.ok()) << file << ": " << s.status();
        EXPECT_GT(s->nsym(), 1u) << file;
        unsigned defined = 0;
        for (unsigned i = 1; i < s->nsym(); ++i) {
            if (s->symtab()[i].st_shndx == 0) continue;
            ++defined;
            const char* n = s->strtab() + s->symtab()[i].st_name;
            EXPECT_EQ(s->Find(n), s->FindLinear(n)) << file << ": " << n;
            EXPECT_NE(s->Find(n), nullptr) << file << ": " << n;
        }
        EXPECT_GT(defined, 0u) << file;
    }
}

}  // namespace
}  // namespace nxmac::loader
