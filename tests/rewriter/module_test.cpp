#include "nxmac/rewriter/module.h"

#include <cstring>
#include <string>
#include <vector>

#include "absl/strings/escaping.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "lz4.h"
#include "nxmac/fs/crypto.h"

namespace nxmac::rewriter {
namespace {

using ::testing::HasSubstr;
using Bytes = std::vector<std::uint8_t>;

void Put32(Bytes* b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) (*b)[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

Bytes Pattern(std::size_t size, std::uint8_t seed) {
    Bytes b(size);
    for (std::size_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>(seed + i * 7);
    return b;
}

// An LZ4 block for n bytes of one value: a literal, a match that repeats it, and the five literals a block ends with
Bytes Lz4Run(std::uint8_t value, std::size_t n) {
    Bytes out;
    std::size_t match = n - 1 - 5 - 4;  // the match's length beyond the 4 its token implies
    out.push_back(static_cast<std::uint8_t>(0x10 | std::min<std::size_t>(match, 15)));
    out.push_back(value);
    out.push_back(1);  // offset 1: the byte before
    out.push_back(0);
    if (match >= 15) {
        for (match -= 15; match >= 255; match -= 255) out.push_back(255);
        out.push_back(static_cast<std::uint8_t>(match));
    }
    out.push_back(0x50);
    out.insert(out.end(), 5, value);
    return out;
}

struct NsoSpec {
    Bytes text = Pattern(0x1800, 1);
    Bytes ro = Pattern(0x900, 2);
    Bytes data = Pattern(0x300, 3);
    std::uint32_t ro_at = 0x2000;
    std::uint32_t data_at = 0x3000;
    std::uint32_t bss = 0x5000;
    bool compress = false;  // each segment then holds one value, which is what BuildNso's LZ4 blocks hold
    bool hashes = true;
};

NsoSpec Compressed() {
    NsoSpec spec;
    spec.text = Bytes(0x1800, 0x41);
    spec.ro = Bytes(0x900, 0x42);
    spec.data = Bytes(0x300, 0x43);
    spec.compress = true;
    return spec;
}

Bytes BuildNso(const NsoSpec& spec) {
    Bytes file(0x100);
    std::memcpy(file.data(), "NSO0", 4);
    const Bytes* segments[3] = {&spec.text, &spec.ro, &spec.data};
    const std::uint32_t at[3] = {0, spec.ro_at, spec.data_at};
    std::uint32_t flags = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const Bytes stored = spec.compress ? Lz4Run((*segments[i])[0], segments[i]->size()) : *segments[i];
        if (spec.compress) flags |= 1U << i;
        if (spec.hashes) flags |= 8U << i;
        Put32(&file, 0x10 + 0x10 * i, static_cast<std::uint32_t>(file.size()));
        Put32(&file, 0x14 + 0x10 * i, at[i]);
        Put32(&file, 0x18 + 0x10 * i, static_cast<std::uint32_t>(segments[i]->size()));
        Put32(&file, 0x60 + 4 * i, static_cast<std::uint32_t>(stored.size()));
        const fs::Sha256Hash hash = fs::Sha256(*segments[i]);
        std::memcpy(file.data() + 0xa0 + 0x20 * i, hash.data(), hash.size());
        file.insert(file.end(), stored.begin(), stored.end());
    }
    Put32(&file, 0xc, flags);
    Put32(&file, 0x3c, spec.bss);
    for (std::size_t i = 0; i < 0x20; ++i) file[0x40 + i] = static_cast<std::uint8_t>(0xa0 + i);
    return file;
}

TEST(Lz4Test, DecodesWhatLz4Compresses) {
    // lz4.block.compress(b'nxmac' * 100 + bytes(range(256)) + b'\0' * 1000 + b'tail', store_size=False)
    std::string block;
    ASSERT_TRUE(
        absl::HexStringToBytes("5f6e786d61630500ffddfff4000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1"
                               "f202122232425262728292a2b"
                               "2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565"
                               "758595a5b5c5d5e5f60616263"
                               "6465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8"
                               "f909192939495969798999a9b"
                               "9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c"
                               "7c8c9cacbcccdcecfd0d1d2d3"
                               "d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfef"
                               "f0000000300ffffffd4500074"
                               "61696c",
                               &block));
    const std::span in(reinterpret_cast<const std::uint8_t*>(block.data()), block.size());  // NOLINT: chars as bytes
    Bytes out(1760);
    ASSERT_EQ(internal::Lz4DecompressBlock(in, out), 1760);
    EXPECT_EQ(
        absl::BytesToHexString(std::string_view(reinterpret_cast<const char*>(fs::Sha256(out).data()), 32)),  // NOLINT
        "49011cb3979a57df4e78ca40aa66afb09154a84e13adb85e24be202bdb84fbe1");
    // More room than the block fills: the bytes written are counted
    Bytes roomy(4000);
    EXPECT_EQ(internal::Lz4DecompressBlock(in, roomy), 1760);
    // Too little room, a block cut short inside a sequence, and a match before the start
    Bytes small(1759);
    EXPECT_EQ(internal::Lz4DecompressBlock(in, small), -1);
    EXPECT_EQ(internal::Lz4DecompressBlock(in.first(3), out), -1);
    const std::uint8_t reach_back[] = {0x10, 'a', 0x02, 0x00};
    EXPECT_EQ(internal::Lz4DecompressBlock(reach_back, out), -1);
    const std::uint8_t zero_offset[] = {0x10, 'a', 0x00, 0x00};
    EXPECT_EQ(internal::Lz4DecompressBlock(zero_offset, out), -1);
    EXPECT_EQ(internal::Lz4DecompressBlock({}, out), 0);
}

TEST(ModuleTest, LoadsAnNso) {
    for (const NsoSpec& spec : {NsoSpec(), Compressed()}) {
        const absl::StatusOr<Module> module = LoadModule(BuildNso(spec), "main");
        ASSERT_TRUE(module.ok()) << module.status();
        EXPECT_EQ(module->image.size(), 0x3300u);
        EXPECT_EQ(module->text_end(), 0x1800u);
        EXPECT_EQ(module->segments[1].offset, 0x2000u);
        EXPECT_EQ(module->segments[2].size, 0x300u);
        EXPECT_EQ(module->bss, 0x5000u);
        EXPECT_EQ(module->BuildId(), "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
        EXPECT_EQ(Bytes(module->image.begin(), module->image.begin() + 0x1800), spec.text);
        EXPECT_EQ(Bytes(module->image.begin() + 0x3000, module->image.end()), spec.data);
        EXPECT_EQ(module->image[0x1800], 0);  // the gap between segments is zeros
    }
}

TEST(ModuleTest, RefusesWhatHorizonRefuses) {
    const auto error = [](const NsoSpec& spec) {
        return std::string(LoadModule(BuildNso(spec), "main").status().message());
    };
    NsoSpec off_page;
    off_page.ro_at = 0x2004;
    EXPECT_THAT(error(off_page), HasSubstr("main: ro segment is at 0x2004, not on a page"));
    NsoSpec overlap;
    overlap.ro_at = 0x1000;
    EXPECT_THAT(error(overlap), HasSubstr("ro segment at 0x1000 overlaps the text segment before it"));
    Bytes short_file = BuildNso(NsoSpec());
    short_file.resize(short_file.size() - 1);
    EXPECT_THAT(std::string(LoadModule(short_file, "main").status().message()), HasSubstr("past the end of the file"));
    EXPECT_THAT(std::string(LoadModule(Bytes(0x40), "main").status().message()), HasSubstr("not an NSO or NRO"));
    Bytes header_only(0x80);
    std::memcpy(header_only.data(), "NSO0", 4);
    EXPECT_THAT(std::string(LoadModule(header_only, "main").status().message()), HasSubstr("header is cut short"));
}

TEST(ModuleTest, ChecksTheSegmentsHashes) {
    Bytes file = BuildNso(NsoSpec());
    file[0x100 + 0x40] ^= 1;
    const absl::StatusOr<Module> damaged = LoadModule(file, "main");
    EXPECT_EQ(damaged.status().code(), absl::StatusCode::kDataLoss);
    EXPECT_THAT(std::string(damaged.status().message()), HasSubstr("text segment does not match its SHA-256"));
    // A module with the check flags cleared is not checked, as on the Switch
    NsoSpec unchecked;
    unchecked.hashes = false;
    file = BuildNso(unchecked);
    file[0x100 + 0x40] ^= 1;
    EXPECT_TRUE(LoadModule(file, "main").ok());
}

TEST(ModuleTest, ACompressedSegmentMustFillItsSize) {
    const NsoSpec spec = Compressed();
    Bytes file = BuildNso(spec);
    // The text's stored size one less: the stream ends short of the header's size
    Put32(&file, 0x60, static_cast<std::uint32_t>(Lz4Run(0x41, spec.text.size()).size()) - 1);
    const absl::StatusOr<Module> module = LoadModule(file, "main");
    EXPECT_EQ(module.status().code(), absl::StatusCode::kDataLoss);
    // More than LZ4 can produce from the stored bytes
    Put32(&file, 0x60, 2);
    EXPECT_THAT(std::string(LoadModule(file, "main").status().message()), HasSubstr("more than LZ4 can produce"));
}

TEST(ModuleTest, NamesWhatItDoesNotDecode) {
    Bytes file = BuildNso(Compressed());
    Put32(&file, 0xc, 0x3f | 0x80);  // ZBIC
    const absl::StatusOr<Module> module = LoadModule(file, "main");
    EXPECT_EQ(module.status().code(), absl::StatusCode::kUnimplemented);
    EXPECT_THAT(std::string(module.status().message()), HasSubstr("ZBIC"));
}

TEST(ModuleTest, LoadsAnNro) {
    Bytes file(0x3000);
    std::memcpy(file.data() + 0x10, "NRO0", 4);
    Put32(&file, 0x18, 0x3000);
    const std::uint32_t segments[3][2] = {{0, 0x1000}, {0x1000, 0x1000}, {0x2000, 0x1000}};
    for (std::size_t i = 0; i < 3; ++i) {
        Put32(&file, 0x20 + 8 * i, segments[i][0]);
        Put32(&file, 0x24 + 8 * i, segments[i][1]);
    }
    Put32(&file, 0x38, 0x2000);
    file[0x40] = 0xab;
    file[0x2fff] = 0x77;
    const absl::StatusOr<Module> module = LoadModule(file, "fighter");
    ASSERT_TRUE(module.ok()) << module.status();
    EXPECT_EQ(module->image, file);
    EXPECT_EQ(module->bss, 0x2000u);
    EXPECT_EQ(module->BuildId().substr(0, 2), "ab");
    // Its segments are whole pages, one after the other, up to its size
    Put32(&file, 0x24, 0x800);
    EXPECT_THAT(std::string(LoadModule(file, "fighter").status().message()), HasSubstr("not whole pages"));
    Put32(&file, 0x24, 0x1000);
    Put32(&file, 0x18, 0x4000);
    EXPECT_THAT(std::string(LoadModule(file, "fighter").status().message()), HasSubstr("not at the module's size"));
    Put32(&file, 0x18, 0x3000);
    Put32(&file, 0x1c, 2);
    EXPECT_EQ(LoadModule(file, "fighter").status().code(), absl::StatusCode::kUnimplemented);
}

}  // namespace
}  // namespace nxmac::rewriter
