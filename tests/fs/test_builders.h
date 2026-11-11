// Test-only builders of the formats nxmac::fs reads: made-up keys, PFS0, RomFS, hash trees, NCAs encrypted with those
// keys, tickets, and update patch sections (BKTR). No game data and no console keys: every byte here is generated.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "nxmac/core/title_id.h"
#include "nxmac/fs/content_meta.h"
#include "nxmac/fs/crypto.h"
#include "nxmac/fs/keys.h"
#include "nxmac/fs/nca.h"

namespace nxmac::fs::testing {

using Bytes = std::vector<std::uint8_t>;

struct File {
    std::string path;  // in a PFS0 its name; in a RomFS "/dir/name"
    Bytes data;
};

// size made-up bytes, the same for the same seed
Bytes PatternBytes(std::size_t size, std::uint32_t seed);

// Lower-case hex of bytes
std::string ToHex(std::span<const std::uint8_t> bytes);

// A prod.keys of made-up values: header_key, key_area_key_application_00 to _03, key_area_key_ocean_00,
// key_area_key_system_00, titlekek_00 to _03, plus comments and keys nxmac skips
std::string TestKeysText();
KeySet TestKeys();

Bytes BuildPfs0(const std::vector<File>& files);
Bytes BuildRomFs(const std::vector<File>& files);

// A section's bytes before encryption, with its hash tree, and the fs header's hash data that describes the tree
struct SectionLayout {
    Bytes plain;
    std::array<std::uint8_t, 0xf8> hash_data{};
};
SectionLayout BuildSha256Section(const Bytes& data, std::uint32_t block_size);
SectionLayout BuildIvfcSection(const Bytes& data, std::uint32_t block_order);

struct SectionSpec {
    NcaFsType fs_type = NcaFsType::kRomFs;
    NcaHashType hash_type = NcaHashType::kHierarchicalIntegrity;
    NcaEncryptionType encryption_type = NcaEncryptionType::kAesCtr;
    SectionLayout layout;
    std::array<std::uint8_t, 0x40> patch_info{};
    std::uint64_t counter_upper = 0x0000123400000000;  // secure value 0x1234, generation 0
    // Encrypts data (the section, padded to 0x200 bytes) in place given its offset in the NCA; unset: AES-CTR
    std::function<void(const Aes128Key& key, std::uint64_t nca_offset, Bytes& data)> encrypt;
};

SectionSpec Pfs0Section(const std::vector<File>& files);
SectionSpec RomFsSection(const std::vector<File>& files);

struct NcaSpec {
    NcaContentType content_type = NcaContentType::kProgram;
    core::TitleId program_id{0x0100000000010000};
    std::uint8_t key_generation = 0;
    int version = 3;
    Aes128Key content_key = {0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
                             0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    // With a rights ID the content key is a title key, which a ticket carries (EncryptTitleKey, BuildTicket)
    std::optional<RightsId> rights_id;
    std::array<std::optional<SectionSpec>, 4> sections;
};

// Encrypted with keys' header_key and key area key (or nothing, with a rights ID)
Bytes BuildNca(const NcaSpec& spec, const KeySet& keys);

Aes128Key EncryptTitleKey(const KeySet& keys, const Aes128Key& title_key, int revision);
// A ticket with an RSA-2048/SHA-256 signature block of zeros
Bytes BuildTicket(const RightsId& rights_id, const Aes128Key& encrypted_title_key, bool common, std::uint8_t revision);

// A packaged content meta (.cnmt)
Bytes BuildContentMeta(core::TitleId title_id, std::uint32_t version, ContentMetaType type,
                       const std::vector<ContentInfo>& contents);

// An update's patch section: patched (the update's RomFS section: IVFC levels and data) laid out as ranges of
// base_plain (the base game's decrypted section) where chunk_size-byte chunks of it occur there, at any chunk offset,
// and patch data for the rest. The patch data is AesCtrEx encrypted in entries of alternating generations (one entry
// left unencrypted); the indirect and AesCtrEx tables follow it, plain AES-CTR.
SectionSpec BuildPatchSection(const Bytes& base_plain, const SectionLayout& patched, std::uint64_t chunk_size);

// The bytes of a bucket tree's table (an L1 node, then entry sets) for entries of entry_size bytes, each starting at
// starts[i], the last ending at end
Bytes BuildBucketTree(const std::vector<Bytes>& entries, const std::vector<std::uint64_t>& starts, std::uint64_t end,
                      std::uint64_t entry_size);

}  // namespace nxmac::fs::testing
