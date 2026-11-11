#include "nxmac/fs/crypto.h"

#include <string>
#include <vector>

#include "absl/strings/escaping.h"
#include "gtest/gtest.h"
#include "test_builders.h"

namespace nxmac::fs {
namespace {

using testing::Bytes;
using testing::PatternBytes;
using testing::ToHex;

template <std::size_t N>
std::array<std::uint8_t, N> FromHex(const char* hex) {
    std::string bytes;
    EXPECT_TRUE(absl::HexStringToBytes(hex, &bytes));
    std::array<std::uint8_t, N> out{};
    std::copy_n(bytes.begin(), std::min(N, bytes.size()), out.begin());
    return out;
}

TEST(CryptoTest, AesEcbMatchesFips197) {
    const auto key = FromHex<16>("000102030405060708090a0b0c0d0e0f");
    const auto plain = FromHex<16>("00112233445566778899aabbccddeeff");
    std::array<std::uint8_t, 16> out{};
    AesEcbEncrypt(key, plain, out);
    EXPECT_EQ(ToHex(out), "69c4e0d86a7b0430d8cdb78070b4c55a");
    AesEcbDecrypt(key, out, out);
    EXPECT_EQ(out, plain);
}

TEST(CryptoTest, AesXtsMatchesIeee1619AtSectorZero) {
    // IEEE 1619 vector 1: both keys and the data unit number zero, where Nintendo's tweak and the standard's agree
    const AesXtsKey key{};
    Bytes data(32, 0);
    AesXtsEncrypt(key, 0, 32, data);
    EXPECT_EQ(ToHex(data), "917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e");
    AesXtsDecrypt(key, 0, 32, data);
    EXPECT_EQ(data, Bytes(32, 0));
}

TEST(CryptoTest, AesXtsTweakIsTheBigEndianSectorNumber) {
    // The first block of a data unit: E_K1(P ^ T) ^ T, T = E_K2(sector as a big-endian 128-bit number)
    AesXtsKey key;
    const Bytes k = PatternBytes(32, 5);
    std::copy(k.begin(), k.end(), key.begin());
    Aes128Key k1;
    Aes128Key k2;
    std::copy_n(key.begin(), 16, k1.begin());
    std::copy_n(key.begin() + 16, 16, k2.begin());
    const Bytes plain = PatternBytes(16, 6);

    std::array<std::uint8_t, 16> tweak{};
    tweak[15] = 0x02;
    tweak[14] = 0x01;  // sector 0x102
    AesEcbEncrypt(k2, tweak, tweak);
    std::array<std::uint8_t, 16> expected{};
    for (int i = 0; i < 16; ++i) expected[i] = plain[i] ^ tweak[i];
    AesEcbEncrypt(k1, expected, expected);
    for (int i = 0; i < 16; ++i) expected[i] ^= tweak[i];

    Bytes data = plain;
    AesXtsEncrypt(key, 0x102, 16, data);
    EXPECT_EQ(ToHex(data), ToHex(expected));
}

TEST(CryptoTest, AesCtrCounterIsTheUpperHalfThenTheBlockNumber) {
    const auto key = FromHex<16>("2b7e151628aed2a6abf7158809cf4f3c");
    Bytes data(32, 0);
    AesCtrTransform(key, 0x0102030405060708, 0x30, data);  // blocks 3 and 4
    for (int block = 0; block < 2; ++block) {
        std::array<std::uint8_t, 16> counter = {1, 2, 3, 4, 5, 6, 7, 8,
                                                0, 0, 0, 0, 0, 0, 0, static_cast<std::uint8_t>(3 + block)};
        AesEcbEncrypt(key, counter, counter);
        EXPECT_EQ(ToHex(std::span(data.data() + 16 * block, 16)), ToHex(counter)) << "block " << block;
    }
}

TEST(CryptoTest, AesCtrCanStartInsideABlock) {
    const Aes128Key key{1, 2, 3};
    Bytes whole = PatternBytes(64, 7);
    const Bytes plain = whole;
    AesCtrTransform(key, 0xaabbccdd00000001, 0x1000, whole);
    Bytes part(plain.begin() + 5, plain.begin() + 45);
    AesCtrTransform(key, 0xaabbccdd00000001, 0x1005, part);
    EXPECT_EQ(part, Bytes(whole.begin() + 5, whole.begin() + 45));
    AesCtrTransform(key, 0xaabbccdd00000001, 0x1000, whole);
    EXPECT_EQ(whole, plain);
}

TEST(CryptoTest, Sha256MatchesFips180) {
    const std::string abc = "abc";
    EXPECT_EQ(ToHex(Sha256(std::span(reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()))),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

}  // namespace
}  // namespace nxmac::fs
