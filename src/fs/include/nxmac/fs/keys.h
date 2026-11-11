// The keys that open a console's content: a prod.keys file, as Lockpick_RCM dumps it, and the title keys of tickets.
// Key values never appear in a message: errors name the key and the line, never what it holds.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "nxmac/fs/crypto.h"

namespace nxmac::fs {

// The key area key an NCA's header selects (its key area key index)
enum class KeyAreaKeyKind : std::uint8_t { kApplication = 0, kOcean = 1, kSystem = 2 };

// A ticket's and an NCA's rights ID: the title ID, then the master key revision in the last byte
using RightsId = std::array<std::uint8_t, 16>;

class KeySet {
public:
    // Master key revisions held: 00 to 3f (firmware 21 brought revision 14)
    static constexpr int kRevisions = 0x40;

    KeySet() = default;

    // The text of a prod.keys file: "name = hex" lines, names in any case, comments after ';' or '#'. The keys that
    // open content are kept (header_key, key_area_key_{application,ocean,system}_XX, titlekek_XX); the others are
    // skipped. A line that is not "name = value", or one of those keys with a value that is not its length of hex
    // digits, is InvalidArgument.
    static absl::StatusOr<KeySet> Parse(std::string_view text);
    static absl::StatusOr<KeySet> Load(const std::string& path);

    // nullptr when the file did not have the key
    const AesXtsKey* header_key() const;
    const Aes128Key* key_area_key(KeyAreaKeyKind kind, int revision) const;
    const Aes128Key* titlekek(int revision) const;

    // A title key from a common ticket, as the ticket holds it: encrypted with the titlekek of the content's revision
    void AddTitleKey(const RightsId& rights_id, const Aes128Key& encrypted_title_key);
    const Aes128Key* encrypted_title_key(const RightsId& rights_id) const;

    // Key names as prod.keys writes them, for messages: key_area_key_application_0a, titlekek_0a
    static std::string KeyAreaKeyName(KeyAreaKeyKind kind, int revision);
    static std::string TitlekekName(int revision);

private:
    std::optional<AesXtsKey> header_key_;
    std::array<std::array<std::optional<Aes128Key>, kRevisions>, 3> key_area_keys_;
    std::array<std::optional<Aes128Key>, kRevisions> titlekeks_;
    absl::flat_hash_map<RightsId, Aes128Key> title_keys_;
};

}  // namespace nxmac::fs
