// The cryptography of the Switch's content formats, on Mbed TLS: AES-128 in the ECB, XTS and CTR modes as NCAs use
// them, and SHA-256 for their hash trees. Each call sets up its own cipher: they are safe from several threads at once.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nxmac::fs {

using Aes128Key = std::array<std::uint8_t, 16>;
using AesXtsKey = std::array<std::uint8_t, 32>;  // the data key, then the tweak key
using Sha256Hash = std::array<std::uint8_t, 32>;

// AES-128-ECB over whole 16-byte blocks; in and out have the same size and may be the same bytes.
void AesEcbDecrypt(const Aes128Key& key, std::span<const std::uint8_t> in, std::span<std::uint8_t> out);
void AesEcbEncrypt(const Aes128Key& key, std::span<const std::uint8_t> in, std::span<std::uint8_t> out);

// AES-128-XTS as Nintendo uses it, in place: every sector_size bytes are one data unit, whose tweak is the sector
// number in big-endian (the IEEE standard's is little-endian). data.size() is a multiple of sector_size.
void AesXtsDecrypt(const AesXtsKey& key, std::uint64_t first_sector, std::size_t sector_size,
                   std::span<std::uint8_t> data);
void AesXtsEncrypt(const AesXtsKey& key, std::uint64_t first_sector, std::size_t sector_size,
                   std::span<std::uint8_t> data);

// AES-128-CTR as NCA sections use it, in place, encrypting and decrypting alike: the counter is counter_upper
// (big-endian) followed by the big-endian block number offset / 16, where offset is the data's offset in the NCA.
// offset need not be a multiple of 16.
void AesCtrTransform(const Aes128Key& key, std::uint64_t counter_upper, std::uint64_t offset,
                     std::span<std::uint8_t> data);

Sha256Hash Sha256(std::span<const std::uint8_t> data);

}  // namespace nxmac::fs
