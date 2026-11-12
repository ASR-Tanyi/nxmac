// The split's Python semantics (src/shader/py.h, archive.h, nvn.h): what struct.unpack_from, slices and bytes.find do
// at the edges, Yaz0, SARC, gzip, zlib and UTF-8 as shader_split.py's readers see them, and the shader keys.
#include "py.h"

#include <zlib.h>

#include <string>
#include <utility>
#include <vector>

#include "archive.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "nvn.h"

namespace nxmac::shader::internal {
namespace {

using ::testing::HasSubstr;
using ::testing::ThrowsMessage;

std::string Le32(std::uint32_t v) {
    return std::string{static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16),
                       static_cast<char>(v >> 24)};
}

TEST(PyTest, UnpackAsStructDoes) {
    const std::string d = "\x01\x02\x03\x04\x05\x06\x07\x08";
    EXPECT_EQ(U32(d, 0), 0x04030201u);
    EXPECT_EQ(U32(d, -4), 0x08070605u);  // a negative offset counts from the end
    EXPECT_EQ(U32Be(d, 0), 0x01020304u);
    EXPECT_EQ(U16(d, 6), 0x0807u);
    EXPECT_EQ(I32(Le32(0xfffffff0), 0), -16);
    try {
        U32(d, 6);
        FAIL();
    } catch (const PyError& e) {
        EXPECT_EQ(e.type(), "error");
        EXPECT_STREQ(e.what(),
                     "unpack_from requires a buffer of at least 10 bytes for unpacking 4 bytes at offset 6 (actual "
                     "buffer size is 8)");
    }
    EXPECT_THROW(U32(d, -9), PyError);
    EXPECT_THROW(Index(d, 8), PyError);
    EXPECT_EQ(Index(d, -1), 8);
}

TEST(PyTest, SlicesClampAsPythonsDo) {
    const std::string d = "abcdef";
    EXPECT_EQ(Slice(d, 2, 4), "cd");
    EXPECT_EQ(Slice(d, 4, 100), "ef");
    EXPECT_EQ(Slice(d, -2, 100), "ef");
    EXPECT_EQ(Slice(d, 5, 2), "");
    EXPECT_EQ(Slice(d, PyInt(1) << 80, PyInt(1) << 81), "");
    EXPECT_EQ(Head(d, -1), "abcde");
}

TEST(PyTest, FindAsBytesFindDoes) {
    const std::string d = "xxBNSHyyBNSH";
    EXPECT_EQ(Find(d, "BNSH"), 2);
    EXPECT_EQ(Find(d, "BNSH", 3), 8);
    EXPECT_EQ(Find(d, "BNSH", 3, 11), -1);
    EXPECT_EQ(Find(d, "", 13), -1);                   // a start past the end finds nothing, not even b''
    EXPECT_EQ(Count("BTCDBTCDBTCD", "BTCDBTCD"), 1);  // count does not overlap
    std::vector<std::int64_t> at;
    FindAll(d, "BNSH", [&](std::int64_t a) { at.push_back(a); });
    EXPECT_EQ(at, (std::vector<std::int64_t>{2, 8}));
}

TEST(PyTest, Hex) {
    EXPECT_EQ(Hex(0), "0x0");
    EXPECT_EQ(Hex(0x1d83000), "0x1d83000");
    EXPECT_EQ(Hex(-16), "-0x10");
    EXPECT_EQ(Hex(PyInt(1) << 64), "0x10000000000000000");
    EXPECT_EQ(Dec(-12), "-12");
}

TEST(ArchiveTest, Yaz0WithBackReferences) {
    // "abc", then 6 bytes from 3 back (overlapping: the period repeats), then 0x12 + 2 = 20 bytes from 1 back
    std::string s = "Yaz0" + std::string("\0\0\0\x1d", 4) + std::string(8, '\0');
    s += static_cast<char>(0xe0);  // three literals, then back-references
    s += "abc";
    s += std::string("\x40\x02", 2);      // length 4 + 2 = 6, distance 3
    s += std::string("\x00\x00\x02", 3);  // length 0x12 + 2 = 20, distance 1
    EXPECT_EQ(Yaz0(s, "t"), "abcabcabc" + std::string(20, 'c'));
}

TEST(ArchiveTest, Yaz0Errors) {
    const std::string head = "Yaz0" + std::string("\0\0\0\x10", 4) + std::string(8, '\0');
    EXPECT_THAT([&] { Yaz0(head + "\xff" + "abc", "f"); },
                ThrowsMessage<Red>("f: the Yaz0 stream ends at 0x14 with 0x3 of its 0x10 bytes decoded"));
    EXPECT_THAT([&] { Yaz0(head + std::string("\x00\x10\x05", 3), "f"); },
                ThrowsMessage<Red>("f: a back-reference at 0x13 reaches 0x6 bytes before the start"));
}

std::string Sarc(const std::vector<std::pair<std::string, std::string>>& members) {
    // the layout sarc_members reads: header, SFAT (count at +6, entries from +12), SFNT, names, data
    const std::size_t hl = 0x14, n = members.size();
    std::string names;
    std::vector<std::uint32_t> attrs;
    for (const auto& [name, data] : members) {
        if (name.empty()) {
            attrs.push_back(0);
            continue;
        }
        attrs.push_back(0x01000000 | static_cast<std::uint32_t>(names.size() / 4));
        names += name + std::string(4 - name.size() % 4, '\0');
    }
    const std::size_t names_at = hl + 12 + 16 * n + 8, doff = names_at + names.size();
    std::string d(doff, '\0');
    d.replace(0, 4, "SARC");
    d[4] = static_cast<char>(hl);
    d.replace(hl + 6, 2, std::string{static_cast<char>(n), 0});
    std::string data;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string e = Le32(attrs[i]) + Le32(static_cast<std::uint32_t>(data.size())) +
                              Le32(static_cast<std::uint32_t>(data.size() + members[i].second.size()));
        d.replace(hl + 12 + 16 * i + 4, 12, e);
        data += members[i].second;
    }
    d.replace(12, 4, Le32(static_cast<std::uint32_t>(doff)));
    d.replace(names_at, names.size(), names);
    return d + data;
}

TEST(ArchiveTest, SarcMembersOneAtATime) {
    std::vector<std::pair<std::string, std::string>> got;
    SarcMembers(Sarc({{"a/b.bin", "12345"}, {"", "678"}}), "s",
                [&](const std::string& n, Bytes m) { got.emplace_back(n, std::string(m)); });
    EXPECT_EQ(got, (std::vector<std::pair<std::string, std::string>>{{"a/b.bin", "12345"}, {"#1", "678"}}));
    // a member out of range is found when its turn comes: the members before it are given first
    std::string bad = Sarc({{"a", "12"}, {"b", "34"}});
    bad.replace(0x14 + 12 + 16 + 12, 4, Le32(0x100));
    got.clear();
    EXPECT_THROW(SarcMembers(bad, "s", [&](const std::string& n, Bytes m) { got.emplace_back(n, std::string(m)); }),
                 Red);
    EXPECT_EQ(got.size(), 1u);
}

TEST(ArchiveTest, Utf8AsPythonDecodesIt) {
    EXPECT_EQ(DecodeUtf8Replace("caf\xc3\xa9"), "caf\xc3\xa9");
    EXPECT_EQ(DecodeUtf8Replace("a\xff"
                                "b"),
              "a\xef\xbf\xbd"
              "b");
    EXPECT_EQ(DecodeUtf8Replace("\xe2\x82"), "\xef\xbf\xbd");  // cut short: the rest, once
    EXPECT_EQ(DecodeUtf8Replace("\xe2\x41"),
              "\xef\xbf\xbd"
              "A");                                                                        // the lead byte alone
    EXPECT_EQ(DecodeUtf8Replace("\xed\xa0\x80"), "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd");  // a surrogate: three
    EXPECT_TRUE(ValidUtf8("caf\xc3\xa9"));
    EXPECT_FALSE(ValidUtf8("\xc0\x80"));
}

std::string Gzip(const std::string& data) {
    z_stream z{};
    deflateInit2(&z, 6, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::string out(deflateBound(&z, data.size()) + 32, '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

TEST(ArchiveTest, GzipMembers) {
    EXPECT_EQ(Ungzip(Gzip("hello ") + std::string(7, '\0') + Gzip("world"), "g"), "hello world");
    std::string damaged = Gzip("hello");
    damaged[damaged.size() - 8] ^= 1;  // its CRC
    EXPECT_THAT([&] { Ungzip(damaged, "g"); },
                ThrowsMessage<Red>(HasSubstr("g: invalid gzip stream (CRC check failed 0x")));
    EXPECT_THAT([&] { Ungzip(Gzip("hello") + "xy", "g"); },
                ThrowsMessage<Red>("g: invalid gzip stream (Not a gzipped file (b'xy'))"));
    const std::string whole = Gzip("hello");
    EXPECT_THAT([&] { Ungzip(whole.substr(0, whole.size() - 4), "g"); },
                ThrowsMessage<Red>(
                    "g: invalid gzip stream (Compressed file ended before the end-of-stream marker was reached)"));
}

TEST(ArchiveTest, InflateAsDecompressobjDoes) {
    const std::string z = [] {
        std::string out(64, '\0');
        uLongf n = out.size();
        compress(reinterpret_cast<Bytef*>(out.data()), &n, reinterpret_cast<const Bytef*>("abcdefgh"), 8);
        out.resize(n);
        return out;
    }();
    const Inflated whole = Inflate(z + "rest");
    EXPECT_EQ(whole.out, "abcdefgh");
    EXPECT_TRUE(whole.eof);
    EXPECT_EQ(whole.unused, 4u);
    const Inflated head = Inflate(z, 3);
    EXPECT_EQ(head.out, "abc");
    EXPECT_FALSE(head.eof);
    EXPECT_FALSE(TryInflate("\x78\x9c\xff\xff"));
    EXPECT_FALSE(Inflate(z.substr(0, 4)).eof);  // a stream cut short is no error, only not at its end
}

TEST(ArchiveTest, FrameSize) {
    EXPECT_EQ(FrameSize(std::string("\x28\xb5\x2f\xfd\x20\x10", 6)), 0x10u);  // single segment, 1-byte size
    EXPECT_EQ(FrameSize(std::string("\x28\xb5\x2f\xfd\x00\x00", 6)), std::nullopt);
    EXPECT_EQ(FrameSize(std::string("\x04\x22\x4d\x18\x08\x40\x34\x12\0\0\0\0\0\0\0", 15)), 0x1234u);
    EXPECT_EQ(PyBytesRepr("a'b\x01"), "b\"a'b\\x01\"");
}

std::string Control(std::uint32_t code_length, std::uint32_t own = 0x880) {
    std::string c(0x880, '\x11');
    c.replace(0, 4, std::string(kCtlMagic));
    c.replace(0x20, 4, Le32(own));
    c.replace(0x704, 4, Le32(code_length));
    return c;
}

std::string Code(int sph_type, std::size_t n = 0x100) {
    std::string k(n, '\x22');
    k.replace(0, 4, std::string(kGfxMagic));
    k.replace(0x30, 4, Le32(0x61 | static_cast<std::uint32_t>(sph_type) << 10));
    return k;
}

TEST(NvnTest, StagesAndChecks) {
    EXPECT_EQ(SphStage(Code(1)), "vs");
    EXPECT_EQ(SphStage(Code(5)), "ps");
    EXPECT_EQ(SphStage(Code(0)), "vs");  // Catherine: Full Body's vertex shaders
    EXPECT_EQ(SphStage(Code(9)), "");
    const std::string k = Code(5);
    EXPECT_EQ(Check("ps", Control(0x100), k), std::nullopt);
    EXPECT_EQ(Check("ps", Control(0x104), k), "control +0x704 does not match the code area length");
    EXPECT_EQ(Check("cs", Control(0x100), k), "code area header magic does not match the stage");
    EXPECT_EQ(Check("ps", "nope", k), "bad control section");
    EXPECT_EQ(CtlLen(Control(0x100, 0x850)), 0x850);
    EXPECT_EQ(CtlLen(Control(0x100, 0x900)), 0x880);
}

TEST(NvnTest, KeysAsTheHostHashesThem) {
    const std::string k = Code(1), c = Control(0x100);
    const std::string key = Skey(c, k, {});
    EXPECT_EQ(key.size(), 16u);
    // bytes past the key's length do not change the key; within the profile's shader_key_skip they do not either
    EXPECT_EQ(Skey(c + "extra", k, {}), key);
    std::string skipped = c;
    skipped[0x780] = 'x';
    EXPECT_NE(Skey(skipped, k, {}), key);
    EXPECT_EQ(Skey(skipped, k, {0x770, 0x78}), Skey(c, k, {0x770, 0x78}));
    EXPECT_TRUE(SameShader(skipped, k, c, k, {0x770, 0x78}));
    EXPECT_FALSE(SameShader(skipped, k, c, k, {}));
}

}  // namespace
}  // namespace nxmac::shader::internal
