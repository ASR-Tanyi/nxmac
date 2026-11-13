#include "nxmac/loader/relocate.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "elf_builder.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nxmac/loader/image.h"
#include "nxmac/loader/layout.h"
#include "nxmac/rewriter/module.h"
#include "nxmac/rewriter/rewrite.h"

namespace nxmac::loader {
namespace {

using ::testing::ElementsAre;
using testing::ElfBuilder;
using ::testing::HasSubstr;
using testing::kData;
using testing::kGlobalFunc;
using testing::kHidden;
using testing::kLocalFunc;
using testing::kWeakFunc;

// What the host answers: a forced takeover of "forced", an implementation of "weak_impl", a stub for anything else
class FakeResolver : public ImportResolver {
public:
    std::uint64_t Forced(std::string_view name) override { return name == "forced" ? 0xf0f0 : 0; }
    absl::StatusOr<std::uint64_t> Import(std::string_view module, std::string_view name) override {
        imports.push_back(absl::StrCat(module, ":", name));
        return name == "weak_impl" ? 0x1111 : 0x2222;
    }
    bool Implements(std::string_view name) override { return name == "weak_impl"; }
    std::vector<std::string> imports;
};

// Mapped images of made-up modules, with their symbols, in load order
struct Loaded {
    std::vector<std::vector<std::uint8_t>> images;
    std::vector<ModuleSymbols> symbols;
    std::vector<LoadedModule> modules;

    Loaded(std::vector<std::pair<std::string, ElfBuilder>> builders) {
        for (auto& [name, b] : builders) images.push_back(b.Build());
        for (std::size_t i = 0; i < images.size(); ++i) {
            absl::StatusOr<ModuleSymbols> s =
                ModuleSymbols::Parse(images[i].data(), images[i].size(), builders[i].first);
            EXPECT_TRUE(s.ok()) << s.status();
            symbols.push_back(*std::move(s));
        }
        for (std::size_t i = 0; i < images.size(); ++i) {
            modules.push_back(
                {names.emplace_back(builders[i].first), images[i].data(), images[i].size(), 0, 0, &symbols[i]});
        }
    }
    std::uint64_t Word(std::size_t m, std::uint64_t off) const {
        std::uint64_t v;
        std::memcpy(&v, images[m].data() + off, 8);
        return v;
    }
    std::uint64_t Base(std::size_t m) const { return reinterpret_cast<std::uint64_t>(images[m].data()); }

    std::vector<std::string> names = std::vector<std::string>(16);  // the views' strings; never reallocated
};

ElfBuilder Main() {
    ElfBuilder b;
    const std::uint32_t foo = b.Sym("foo", 1, 0x100);
    const std::uint32_t shared = b.Sym("shared", 1, 0x200);
    const std::uint32_t local = b.Sym("local_thing", 1, 0x300, kLocalFunc);
    const std::uint32_t hidden = b.Sym("hidden_thing", 1, 0x310, kGlobalFunc, kHidden);
    const std::uint32_t sdk_func = b.Sym("sdk_func", 0, 0);
    const std::uint32_t weak_missing = b.Sym("weak_missing", 0, 0, kWeakFunc);
    const std::uint32_t weak_impl = b.Sym("weak_impl", 0, 0, kWeakFunc);
    const std::uint32_t missing = b.Sym("missing", 0, 0);
    const std::uint32_t forced = b.Sym("forced", 1, 0x400);
    b.Rela(kData + 0x00, 0, kRAarch64Relative, 0x123);
    b.Rela(kData + 0x08, foo, kRAarch64Abs64, 8);
    b.Rela(kData + 0x10, sdk_func, kRAarch64GlobDat);
    b.Rela(kData + 0x20, local, kRAarch64GlobDat);
    b.Rela(kData + 0x28, hidden, kRAarch64GlobDat);
    b.Rela(kData + 0x30, weak_missing, kRAarch64GlobDat);
    b.Rela(kData + 0x38, weak_impl, kRAarch64GlobDat);
    b.Rela(kData + 0x48, forced, kRAarch64GlobDat);
    b.Rela(kData + 0x50, shared, kRAarch64GlobDat);
    b.JmpRel(kData + 0x18, sdk_func);
    b.JmpRel(kData + 0x40, missing);
    // RELR: a start at +0x100, then a bitmap for the slots after it: bits 1 and 3 (slots 0 and 2 from +0x108)
    b.Relr({kData + 0x100, 0b1011});
    b.Word(kData + 0x100, 0x10);
    b.Word(kData + 0x108, 0x20);
    b.Word(kData + 0x110, 0x77);  // not in the bitmap
    b.Word(kData + 0x118, 0x30);
    return b;
}

ElfBuilder Sdk() {
    ElfBuilder b;
    const std::uint32_t sdk_func = b.Sym("sdk_func", 1, 0x50);
    const std::uint32_t shared = b.Sym("shared", 1, 0x60);
    b.Sym("local_thing", 1, 0x70);
    b.Sym("hidden_thing", 1, 0x80);
    b.Rela(kData + 0x00, shared, kRAarch64GlobDat);
    b.JmpRel(kData + 0x08, sdk_func);
    return b;
}

TEST(RelocateTest, BindsAsHorizonsRtld) {
    Loaded l({{"main", Main()}, {"sdk", Sdk()}});
    FakeResolver host;
    const absl::StatusOr<RelocationReport> r = Relocate(l.modules, 0, host);
    ASSERT_TRUE(r.ok()) << r.status();
    const std::uint64_t main = l.Base(0);
    const std::uint64_t sdk = l.Base(1);
    EXPECT_EQ(l.Word(0, kData + 0x00), main + 0x123);  // RELATIVE
    EXPECT_EQ(l.Word(0, kData + 0x08), main + 0x108);  // ABS64 with its addend
    EXPECT_EQ(l.Word(0, kData + 0x10), sdk + 0x50);    // an import another module defines
    EXPECT_EQ(l.Word(0, kData + 0x18), sdk + 0x50);    // ... through the PLT
    EXPECT_EQ(l.Word(0, kData + 0x20), main + 0x300);  // a local symbol binds to itself, though sdk defines the name
    EXPECT_EQ(l.Word(0, kData + 0x28), main + 0x310);  // so does a hidden one
    EXPECT_EQ(l.Word(0, kData + 0x30), 0u);            // an undefined weak symbol nothing defines
    EXPECT_EQ(l.Word(0, kData + 0x38), 0x1111u);       // an undefined weak symbol the host implements
    EXPECT_EQ(l.Word(0, kData + 0x40), 0x2222u);       // an import nothing defines: the host's stub
    EXPECT_EQ(l.Word(0, kData + 0x48), 0xf0f0u);       // a forced takeover over the module's own definition
    EXPECT_EQ(l.Word(0, kData + 0x50), main + 0x200);  // main defines "shared" first
    EXPECT_EQ(l.Word(0, kData + 0x100), main + 0x10);  // RELR: the start...
    EXPECT_EQ(l.Word(0, kData + 0x108), main + 0x20);  // ... and the bitmap's slots
    EXPECT_EQ(l.Word(0, kData + 0x110), 0x77u);
    EXPECT_EQ(l.Word(0, kData + 0x118), main + 0x30);
    EXPECT_EQ(r->relative, 1);
    EXPECT_EQ(r->relr, 3u);
    EXPECT_EQ(r->abs_glob, 8);
    EXPECT_EQ(r->jump, 2);
    EXPECT_EQ(r->weak0, 1);
    EXPECT_EQ(r->other, 0);
    EXPECT_THAT(r->lines, ElementsAre("main: undefined weak weak_missing bound to 0",
                                      "main: reloc relative=1 relr=3 abs/glob=8 jump=2"));
    EXPECT_THAT(host.imports, ElementsAre("main:weak_impl", "main:missing"));
}

TEST(RelocateTest, AnEarlierModulesDefinitionWins) {
    Loaded l({{"main", Main()}, {"sdk", Sdk()}});
    FakeResolver host;
    const absl::StatusOr<RelocationReport> r = Relocate(l.modules, 1, host);
    ASSERT_TRUE(r.ok()) << r.status();
    EXPECT_EQ(l.Word(1, kData + 0x00), l.Base(0) + 0x200);  // main's "shared", not sdk's own
    EXPECT_EQ(l.Word(1, kData + 0x08), l.Base(1) + 0x50);   // sdk's own sdk_func: main only imports it
    EXPECT_THAT(r->lines, ElementsAre("sdk: shared bound to an earlier module's definition (main)",
                                      "sdk: reloc relative=0 relr=0 abs/glob=1 jump=1 (some bound to an earlier "
                                      "module's definition)"));
}

TEST(RelocateTest, EightLinesAndACount) {
    ElfBuilder first;
    ElfBuilder second;
    for (int i = 0; i < 10; ++i) {
        first.Sym(absl::StrCat("s", i), 1, 0x10 * i);
        second.Rela(kData + 8 * i, second.Sym(absl::StrCat("s", i), 1, 0x20 * i), kRAarch64GlobDat);
    }
    Loaded l({{"main", first}, {"sdk", second}});
    FakeResolver host;
    const absl::StatusOr<RelocationReport> r = Relocate(l.modules, 1, host);
    ASSERT_TRUE(r.ok()) << r.status();
    EXPECT_EQ(r->other, 10);
    ASSERT_EQ(r->lines.size(), 10u);
    EXPECT_EQ(r->lines[7], "sdk: s7 bound to an earlier module's definition (main)");
    EXPECT_EQ(r->lines[9], "sdk: 10 bound to an earlier module's definition in all");
}

TEST(RelocateTest, WhatStops) {
    FakeResolver host;
    ElfBuilder bitmap_first;
    bitmap_first.Relr({0b11});
    Loaded a({{"main", bitmap_first}});
    EXPECT_THAT(Relocate(a.modules, 0, host).status().message(),
                HasSubstr("main: DT_RELR bitmap entry with no start before it"));
    ElfBuilder far_place;
    far_place.Rela(0x100000, 0, kRAarch64Relative, 1);
    Loaded b({{"main", far_place}});
    EXPECT_THAT(Relocate(b.modules, 0, host).status().message(),
                HasSubstr("main: relocation at 0x100000 is outside the image"));
    ElfBuilder no_symbol;
    no_symbol.Rela(kData, 5, kRAarch64GlobDat);
    Loaded c({{"main", no_symbol}});
    EXPECT_THAT(Relocate(c.modules, 0, host).status().message(),
                HasSubstr("main: relocation at 0x8000 names symbol 5 of 1"));
}

// The rewriter's fixtures, rewritten as nso writes them, mapped and relocated together: every RELATIVE place holds the
// base plus its addend, and the counts are the tables'
TEST(RelocateTest, RelocatesRewrittenModules) {
    const std::vector<std::pair<std::string, std::string>> files = {
        {"main", "g.nso"}, {"subsdk0", "data.nso"}, {"sdk", "sys.nso"}};
    std::vector<ModuleImage> images;
    std::vector<ModuleSymbols> symbols;
    for (const auto& [name, file] : files) {
        const absl::StatusOr<rewriter::Module> module =
            rewriter::ReadModule((std::filesystem::path(NXMAC_REWRITER_FIXTURES) / file).string());
        ASSERT_TRUE(module.ok()) << module.status();
        rewriter::RewriteOptions options;
        options.name = name;
        const absl::StatusOr<rewriter::RewriteResult> result = rewriter::Rewrite(*module, options);
        ASSERT_TRUE(result.ok()) << result.status();
        const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / "relocate_test";
        std::filesystem::create_directories(dir);
        std::ofstream(dir / (name + ".patched.bin"), std::ios::binary)
            .write(reinterpret_cast<const char*>(result->patched.data()),
                   static_cast<std::streamsize>(result->patched.size()));
        const absl::StatusOr<ModuleLayout> layout = ParseLayout(result->fixups, name);
        ASSERT_TRUE(layout.ok()) << layout.status();
        absl::StatusOr<ModuleImage> image = MapModule(dir, name, *layout, {11, 12, 13, 14});
        ASSERT_TRUE(image.ok()) << image.status();
        images.push_back(*std::move(image));
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        absl::StatusOr<ModuleSymbols> s = ModuleSymbols::Parse(images[i].base(), images[i].size(), files[i].first);
        ASSERT_TRUE(s.ok()) << s.status();
        symbols.push_back(*std::move(s));
    }
    std::vector<LoadedModule> modules;
    modules.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        modules.push_back({files[i].first, images[i].base(), images[i].size(), 0, images[i].patch_len(), &symbols[i]});
    }
    FakeResolver host;
    int relocated = 0;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        const absl::StatusOr<RelocationReport> r = Relocate(modules, i, host);
        ASSERT_TRUE(r.ok()) << files[i].second << ": " << r.status();
        relocated += r->relative + r->abs_glob + r->jump + static_cast<int>(r->relr);
        // Every RELATIVE entry of DT_RELA, read back; every entry of both tables counted once
        std::uint64_t rela = 0, relasz = 0, pltsz = 0;
        for (const Elf64Dyn* d = symbols[i].dyn(); d->d_tag != 0; ++d) {
            if (d->d_tag == kDtRela) rela = d->d_val;
            if (d->d_tag == kDtRelaSz) relasz = d->d_val;
            if (d->d_tag == kDtPltRelSz) pltsz = d->d_val;
        }
        EXPECT_EQ(static_cast<std::uint64_t>(r->relative + r->abs_glob + r->jump), (relasz + pltsz) / sizeof(Elf64Rela))
            << files[i].second;
        int relative = 0;
        for (std::uint64_t k = 0; k < relasz / sizeof(Elf64Rela); ++k) {
            Elf64Rela e;
            std::memcpy(&e, images[i].base() + rela + k * sizeof e, sizeof e);
            if ((e.r_info & 0xffffffff) != kRAarch64Relative) continue;
            ++relative;
            std::uint64_t v;
            std::memcpy(&v, images[i].base() + e.r_offset, 8);
            EXPECT_EQ(v, reinterpret_cast<std::uint64_t>(images[i].base()) + static_cast<std::uint64_t>(e.r_addend));
        }
        EXPECT_EQ(r->relative, relative) << files[i].second;
    }
    EXPECT_EQ(relocated, 567 + 163);  // g.nso's tables; the others have none
}

}  // namespace
}  // namespace nxmac::loader
