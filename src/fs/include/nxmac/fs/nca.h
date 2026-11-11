// NCAs (Nintendo Content Archives): the encrypted containers a title's content comes in. A header (AES-XTS with the
// console's header_key) describes up to four sections; each is AES-CTR encrypted with the content key, which the
// header's key area holds (encrypted with a key area key) or a ticket's title key replaces, and checked by a hash tree
// whose master hash is in the header. An update's Program NCA patches the base game's data section (BKTR) instead of
// holding all of it.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "nxmac/core/title_id.h"
#include "nxmac/fs/crypto.h"
#include "nxmac/fs/keys.h"
#include "nxmac/fs/storage.h"

namespace nxmac::fs {

enum class NcaContentType : std::uint8_t {
    kProgram = 0,
    kMeta = 1,
    kControl = 2,
    kManual = 3,
    kData = 4,
    kPublicData = 5
};
enum class NcaFsType : std::uint8_t { kRomFs = 0, kPartitionFs = 1 };
enum class NcaHashType : std::uint8_t {
    kAuto = 0,
    kNone = 1,
    kHierarchicalSha256 = 2,     // PartitionFs: one table of block hashes
    kHierarchicalIntegrity = 3,  // RomFS: IVFC, six levels
    kAutoSha3 = 4,
    kHierarchicalSha3256 = 5,
    kHierarchicalIntegritySha3 = 6,
};
enum class NcaEncryptionType : std::uint8_t {
    kAuto = 0,
    kNone = 1,
    kAesXts = 2,
    kAesCtr = 3,
    kAesCtrEx = 4,  // an update's patch section (BKTR)
    kAesCtrSkipLayerHash = 5,
    kAesCtrExSkipLayerHash = 6,
};

// The sections of each content type: a Program NCA has its code (ExeFS), data (RomFS) and logo; the others one section.
inline constexpr int kProgramCodeSection = 0;
inline constexpr int kProgramDataSection = 1;
inline constexpr int kProgramLogoSection = 2;

struct NcaSection {
    std::uint64_t offset = 0;  // in the NCA
    std::uint64_t size = 0;
    NcaFsType fs_type = NcaFsType::kRomFs;
    NcaHashType hash_type = NcaHashType::kNone;
    NcaEncryptionType encryption_type = NcaEncryptionType::kNone;
    std::array<std::uint8_t, 0xf8> hash_data{};   // HierarchicalSha256 or IVFC info, as stored
    std::array<std::uint8_t, 0x40> patch_info{};  // an AesCtrEx section's indirect and AesCtrEx tables, as stored
    std::uint64_t counter_upper = 0;              // the upper half of the AES-CTR counter: secure value, generation
    bool sparse = false;                          // has a sparse layer (a base game whose data the update carries)
    bool compressed = false;                      // has a compression layer
};

struct NcaHeader {
    int version = 3;                     // NCA3, or NCA2
    std::uint8_t distribution_type = 0;  // 0 download, 1 game card
    NcaContentType content_type = NcaContentType::kProgram;
    std::uint8_t key_generation = 0;  // the larger of the header's two fields
    std::uint8_t key_area_key_index = 0;
    std::uint64_t content_size = 0;
    core::TitleId program_id;
    std::uint32_t content_index = 0;
    std::uint32_t sdk_addon_version = 0;
    RightsId rights_id{};
    std::array<std::optional<NcaSection>, 4> sections;

    bool has_rights_id() const;
    // The master key revision of the content key's keys: key generations 0 and 1 are both revision 0
    int master_key_revision() const { return key_generation > 0 ? key_generation - 1 : 0; }
};

class Nca {
public:
    // Reads and decrypts the header with keys.header_key() (FailedPrecondition without it; InvalidArgument when the
    // bytes are no NCA, which a wrong header_key also gives), checks each section header against its hash, and finds
    // the content key. A missing content key does not fail Open: it fails the reads of the sections, and key_status()
    // names the key.
    static absl::StatusOr<Nca> Open(const KeySet& keys, StoragePtr storage);

    const NcaHeader& header() const { return header_; }
    bool has_section(int index) const;
    // OK when the content key was found; otherwise FailedPrecondition, naming the key prod.keys lacks or the ticket
    // that is missing
    const absl::Status& key_status() const { return key_status_; }

    // Section index decrypted, below its hash tree: what an update's patch is laid over.
    absl::StatusOr<StoragePtr> OpenRawSection(int index) const;
    // Section index decrypted and checked against its hash tree: a PFS0 image (code, control data of old games, logo,
    // meta) or a RomFS image. A section an update patches (AesCtrEx) opens only with OpenPatchedSection.
    absl::StatusOr<StoragePtr> OpenSection(int index) const;

private:
    friend absl::StatusOr<StoragePtr> OpenPatchedSection(const Nca& base, const Nca& patch, int index);

    // nullptr when there is no such section
    const NcaSection* section(int index) const;

    StoragePtr storage_;
    NcaHeader header_;
    std::optional<Aes128Key> content_key_;
    absl::Status key_status_;
};

// Section index of the base game's NCA with the update's NCA (patch, an AesCtrEx section) laid over it, checked against
// the update's hash tree: the update's version of the section. base may lack the section, when the update adds it.
absl::StatusOr<StoragePtr> OpenPatchedSection(const Nca& base, const Nca& patch, int index);

}  // namespace nxmac::fs
