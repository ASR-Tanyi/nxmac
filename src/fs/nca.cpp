#include "nxmac/fs/nca.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/escaping.h"
#include "absl/strings/str_format.h"
#include "bytes.h"
#include "integrity.h"
#include "section_storage.h"

namespace nxmac::fs {
namespace {

using internal::LoadLe;

constexpr std::uint64_t kHeaderSize = 0xc00;
constexpr std::uint64_t kXtsSector = 0x200;
constexpr std::uint64_t kMediaUnit = 0x200;  // the unit of the section table's offsets
constexpr std::uint64_t kFsHeaders = 0x400;
constexpr std::uint64_t kFsHeaderSize = 0x200;

// The firmware that brought each master key revision (Atmosphère's and hactool's tables), to say how old keys are
constexpr std::string_view kFirmware[] = {"1.0.0",  "3.0.0",  "3.0.1",  "4.0.0",  "5.0.0",  "6.0.0",  "6.2.0",
                                          "7.0.0",  "8.1.0",  "9.0.0",  "9.1.0",  "12.1.0", "13.0.0", "14.0.0",
                                          "15.0.0", "16.0.0", "17.0.0", "18.0.0", "19.0.0", "20.0.0", "21.0.0"};

absl::Status MissingKey(const std::string& name, int revision) {
    const std::string firmware = static_cast<std::size_t>(revision) < std::size(kFirmware)
                                     ? absl::StrFormat("firmware %s", kFirmware[revision])
                                     : absl::StrFormat("a firmware after %s", kFirmware[std::size(kFirmware) - 1]);
    return absl::FailedPreconditionError(absl::StrFormat(
        "prod.keys has no %s (master key %02x, which came with %s): the keys are older than the content; "
        "dump them again from a console on %s or later",
        name, revision, firmware, firmware));
}

NcaSection ParseSection(std::span<const std::uint8_t> fs_header, std::uint64_t offset, std::uint64_t size) {
    NcaSection s;
    s.offset = offset;
    s.size = size;
    s.fs_type = static_cast<NcaFsType>(fs_header[2]);
    s.hash_type = static_cast<NcaHashType>(fs_header[3]);
    s.encryption_type = static_cast<NcaEncryptionType>(fs_header[4]);
    std::copy_n(fs_header.begin() + 0x8, s.hash_data.size(), s.hash_data.begin());
    std::copy_n(fs_header.begin() + 0x100, s.patch_info.size(), s.patch_info.begin());
    s.counter_upper = LoadLe<std::uint64_t>(fs_header, 0x140);
    s.sparse = LoadLe<std::uint16_t>(fs_header, 0x148 + 0x28) != 0;  // the sparse info's generation
    s.compressed = LoadLe<std::uint64_t>(fs_header, 0x178) != 0 && LoadLe<std::uint64_t>(fs_header, 0x180) != 0;
    return s;
}

// The section's hash tree over its raw (decrypted) bytes
absl::StatusOr<StoragePtr> CheckedSection(StoragePtr raw, const NcaSection& section, int index) {
    NcaHashType hash = section.hash_type;
    if (hash == NcaHashType::kAuto) {
        hash = section.fs_type == NcaFsType::kPartitionFs ? NcaHashType::kHierarchicalSha256
                                                          : NcaHashType::kHierarchicalIntegrity;
    }
    switch (hash) {
        case NcaHashType::kNone:
            return raw;
        case NcaHashType::kHierarchicalSha256:
            return internal::OpenHierarchicalSha256(raw, section.hash_data);
        case NcaHashType::kHierarchicalIntegrity:
            return internal::OpenHierarchicalIntegrity(raw, section.hash_data);
        default:
            return absl::UnimplementedError(
                absl::StrFormat("NCA section %d: hash type %d is not read yet", index, static_cast<int>(hash)));
    }
}

}  // namespace

bool NcaHeader::has_rights_id() const {
    return std::any_of(rights_id.begin(), rights_id.end(), [](std::uint8_t b) { return b != 0; });
}

absl::StatusOr<Nca> Nca::Open(const KeySet& keys, StoragePtr storage) {
    const AesXtsKey* header_key = keys.header_key();
    if (header_key == nullptr) return absl::FailedPreconditionError("prod.keys has no header_key");
    if (storage->size() < kHeaderSize) return absl::InvalidArgumentError("too small for an NCA");
    absl::StatusOr<std::vector<std::uint8_t>> read = ReadBytes(*storage, 0, kHeaderSize);
    if (!read.ok()) return read.status();
    std::vector<std::uint8_t>& h = *read;

    // The signatures and the header proper are sectors 0 and 1; NCA3 encrypts the section headers as sectors 2 to 5,
    // NCA2 each as sector 0
    AesXtsDecrypt(*header_key, 0, kXtsSector, std::span(h.data(), kFsHeaders));
    const std::string_view magic(reinterpret_cast<const char*>(h.data() + 0x200), 4);
    Nca nca;
    if (magic == "NCA3") {
        nca.header_.version = 3;
        AesXtsDecrypt(*header_key, 2, kXtsSector, std::span(h.data() + kFsHeaders, kHeaderSize - kFsHeaders));
    } else if (magic == "NCA2") {
        nca.header_.version = 2;
        for (std::uint64_t i = 0; i < 4; ++i) {
            AesXtsDecrypt(*header_key, 0, kXtsSector,
                          std::span(h.data() + kFsHeaders + i * kFsHeaderSize, kFsHeaderSize));
        }
    } else if (magic == "NCA0" || magic == "NCA1") {
        return absl::UnimplementedError(absl::StrFormat("%s (early firmware) is not read", magic));
    } else {
        return absl::InvalidArgumentError(
            "not an NCA: its header decrypts to no NCA magic (a damaged file, or a header_key of other consoles)");
    }

    NcaHeader& header = nca.header_;
    header.distribution_type = h[0x204];
    if (h[0x205] > static_cast<std::uint8_t>(NcaContentType::kPublicData)) {
        return absl::InvalidArgumentError(absl::StrFormat("NCA of content type %d", h[0x205]));
    }
    header.content_type = static_cast<NcaContentType>(h[0x205]);
    header.key_generation = std::max(h[0x206], h[0x220]);
    header.key_area_key_index = h[0x207];
    header.content_size = LoadLe<std::uint64_t>(h, 0x208);
    header.program_id = core::TitleId(LoadLe<std::uint64_t>(h, 0x210));
    header.content_index = LoadLe<std::uint32_t>(h, 0x218);
    header.sdk_addon_version = LoadLe<std::uint32_t>(h, 0x21c);
    std::copy_n(h.begin() + 0x230, header.rights_id.size(), header.rights_id.begin());

    for (int i = 0; i < 4; ++i) {
        const auto start = LoadLe<std::uint32_t>(h, 0x240 + i * 0x10);
        const auto end = LoadLe<std::uint32_t>(h, 0x244 + i * 0x10);
        if (end <= start) continue;
        const std::uint64_t offset = start * kMediaUnit;
        const std::uint64_t size = (end - start) * kMediaUnit;
        if (absl::Status s = CheckRange(offset, size, storage->size()); !s.ok()) {
            return absl::DataLossError(
                absl::StrFormat("NCA section %d ends past the end of the file (cut short?): %s", i, s.message()));
        }
        const std::span<const std::uint8_t> fs_header(h.data() + kFsHeaders + i * kFsHeaderSize, kFsHeaderSize);
        if (std::memcmp(Sha256(fs_header).data(), h.data() + 0x280 + i * 0x20, 0x20) != 0) {
            return absl::DataLossError(absl::StrFormat("NCA section %d: its header fails its hash check", i));
        }
        header.sections[i] = ParseSection(fs_header, offset, size);
    }

    // The content key: a title key from a ticket, or the key area's AES-CTR key
    const int revision = header.master_key_revision();
    if (header.has_rights_id()) {
        const Aes128Key* title_key = keys.encrypted_title_key(header.rights_id);
        const Aes128Key* kek = keys.titlekek(revision);
        if (title_key == nullptr) {
            nca.key_status_ = absl::FailedPreconditionError(
                absl::StrFormat("no title key for rights ID %s: the package has no common ticket for it",
                                absl::BytesToHexString(std::string_view(
                                    reinterpret_cast<const char*>(header.rights_id.data()), header.rights_id.size()))));
        } else if (kek == nullptr) {
            nca.key_status_ = MissingKey(KeySet::TitlekekName(revision), revision);
        } else {
            Aes128Key key;
            AesEcbDecrypt(*kek, *title_key, key);
            nca.content_key_ = key;
        }
    } else {
        const auto kind = static_cast<KeyAreaKeyKind>(header.key_area_key_index);
        const Aes128Key* kak = header.key_area_key_index <= 2 ? keys.key_area_key(kind, revision) : nullptr;
        if (header.key_area_key_index > 2) {
            nca.key_status_ =
                absl::InvalidArgumentError(absl::StrFormat("NCA of key area key index %d", header.key_area_key_index));
        } else if (kak == nullptr) {
            nca.key_status_ = MissingKey(KeySet::KeyAreaKeyName(kind, revision), revision);
        } else {
            std::array<std::uint8_t, 0x40> area;
            AesEcbDecrypt(*kak, std::span(h.data() + 0x300, area.size()), area);
            Aes128Key key;
            std::copy_n(area.begin() + 0x20, key.size(), key.begin());  // the third key: AES-CTR
            nca.content_key_ = key;
        }
    }
    nca.storage_ = std::move(storage);
    return nca;
}

const NcaSection* Nca::section(int index) const {
    if (index < 0 || index >= 4) return nullptr;
    const std::optional<NcaSection>& section = header_.sections[index];
    return section ? &*section : nullptr;
}

bool Nca::has_section(int index) const { return section(index) != nullptr; }

absl::StatusOr<StoragePtr> Nca::OpenRawSection(int index) const {
    const NcaSection* found = section(index);
    if (found == nullptr) return absl::NotFoundError(absl::StrFormat("the NCA has no section %d", index));
    const NcaSection& section = *found;
    if (section.sparse) {
        return absl::UnimplementedError(absl::StrFormat(
            "NCA section %d is sparse (its update carries the rest of its data): sparse layers are not read yet",
            index));
    }
    if (section.compressed) {
        return absl::UnimplementedError(
            absl::StrFormat("NCA section %d is compressed: compression layers are not read yet", index));
    }
    absl::StatusOr<StoragePtr> raw = MakeSubStorage(storage_, section.offset, section.size);
    if (!raw.ok()) return raw.status();
    switch (section.encryption_type) {
        case NcaEncryptionType::kNone:
            return raw;
        case NcaEncryptionType::kAesCtr:
            if (!content_key_) return key_status_;  // not OK whenever there is no content key
            return internal::MakeAesCtrStorage(*std::move(raw), *content_key_, section.counter_upper, section.offset);
        case NcaEncryptionType::kAesCtrEx:
            return absl::FailedPreconditionError(absl::StrFormat(
                "NCA section %d is an update's patch: it opens over the base game's NCA (OpenPatchedSection)", index));
        default:
            return absl::UnimplementedError(absl::StrFormat("NCA section %d: encryption type %d is not read yet", index,
                                                            static_cast<int>(section.encryption_type)));
    }
}

absl::StatusOr<StoragePtr> Nca::OpenSection(int index) const {
    absl::StatusOr<StoragePtr> raw = OpenRawSection(index);
    if (!raw.ok()) return raw.status();
    return CheckedSection(*std::move(raw), *section(index), index);
}

absl::StatusOr<StoragePtr> OpenPatchedSection(const Nca& base, const Nca& patch, int index) {
    const NcaSection* found = patch.section(index);
    if (found == nullptr) return absl::NotFoundError(absl::StrFormat("the update's NCA has no section %d", index));
    const NcaSection& section = *found;
    // A section the update holds whole (its code, typically) needs no base
    if (section.encryption_type != NcaEncryptionType::kAesCtrEx) return patch.OpenSection(index);
    if (!patch.content_key_) return patch.key_status_;
    const Aes128Key& key = *patch.content_key_;

    // The patch info: the indirect table (offset, size, tree header), then the AesCtrEx table (offset, size, tree
    // header). Both tables are plain AES-CTR; the data before them is AesCtrEx.
    const std::span<const std::uint8_t> info(section.patch_info);
    const auto indirect_offset = LoadLe<std::uint64_t>(info, 0);
    const auto indirect_size = LoadLe<std::uint64_t>(info, 8);
    const auto ex_offset = LoadLe<std::uint64_t>(info, 0x20);
    const auto ex_size = LoadLe<std::uint64_t>(info, 0x28);
    if (!CheckRange(indirect_offset, indirect_size, section.size).ok() ||
        !CheckRange(ex_offset, ex_size, section.size).ok() || ex_offset < indirect_offset) {
        return absl::InvalidArgumentError(absl::StrFormat("NCA section %d: its patch tables lie outside it", index));
    }
    absl::StatusOr<StoragePtr> raw = MakeSubStorage(patch.storage_, section.offset, section.size);
    if (!raw.ok()) return raw.status();
    absl::StatusOr<StoragePtr> tables_raw =
        MakeSubStorage(*raw, indirect_offset, ex_offset + ex_size - indirect_offset);
    if (!tables_raw.ok()) return tables_raw.status();
    const StoragePtr tables = internal::MakeAesCtrStorage(*std::move(tables_raw), key, section.counter_upper,
                                                          section.offset + indirect_offset);

    absl::StatusOr<StoragePtr> ex_table = MakeSubStorage(tables, ex_offset - indirect_offset, ex_size);
    if (!ex_table.ok()) return ex_table.status();
    absl::StatusOr<internal::BucketTreeEntries> ex_tree =
        internal::ReadBucketTree(**ex_table, info.subspan(0x30, 0x10), internal::kAesCtrExEntrySize);
    if (!ex_tree.ok()) return ex_tree.status();
    absl::StatusOr<std::vector<internal::AesCtrExEntry>> ex_entries = internal::ParseAesCtrExEntries(*ex_tree);
    if (!ex_entries.ok()) return ex_entries.status();
    absl::StatusOr<StoragePtr> patch_data = internal::MakeAesCtrExStorage(
        *raw, *std::move(ex_entries), ex_tree->end_offset, key, section.counter_upper, section.offset);
    if (!patch_data.ok()) return patch_data.status();

    absl::StatusOr<StoragePtr> indirect_table = MakeSubStorage(tables, 0, indirect_size);
    if (!indirect_table.ok()) return indirect_table.status();
    absl::StatusOr<internal::BucketTreeEntries> indirect_tree =
        internal::ReadBucketTree(**indirect_table, info.subspan(0x10, 0x10), internal::kIndirectEntrySize);
    if (!indirect_tree.ok()) return indirect_tree.status();
    absl::StatusOr<std::vector<internal::IndirectEntry>> indirect_entries =
        internal::ParseIndirectEntries(*indirect_tree);
    if (!indirect_entries.ok()) return indirect_entries.status();

    StoragePtr base_raw = MakeMemoryStorage({});
    if (base.has_section(index)) {
        absl::StatusOr<StoragePtr> opened = base.OpenRawSection(index);
        if (!opened.ok()) {
            return absl::Status(opened.status().code(),
                                absl::StrFormat("the base game's section %d: %s", index, opened.status().message()));
        }
        base_raw = *std::move(opened);
    }
    absl::StatusOr<StoragePtr> patched = internal::MakeIndirectStorage(
        {std::move(base_raw), *std::move(patch_data)}, *std::move(indirect_entries), indirect_tree->end_offset);
    if (!patched.ok()) return patched.status();
    return CheckedSection(*std::move(patched), section, index);
}

}  // namespace nxmac::fs
