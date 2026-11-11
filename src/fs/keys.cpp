#include "nxmac/fs/keys.h"

#include <cstddef>
#include <span>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "nxmac/fs/storage.h"

namespace nxmac::fs {
namespace {

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// out.size() bytes from exactly twice as many hex digits
bool DecodeHex(std::string_view hex, std::span<std::uint8_t> out) {
    if (hex.size() != out.size() * 2) return false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = HexDigit(hex[2 * i]);
        const int lo = HexDigit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return true;
}

// The revision of a "<prefix>XX" name: its two hex digits; -1 when name is not one (key_area_key_application_source)
int Revision(std::string_view name, std::string_view prefix) {
    if (!absl::StartsWith(name, prefix) || name.size() != prefix.size() + 2) return -1;
    const int hi = HexDigit(name[prefix.size()]);
    const int lo = HexDigit(name[prefix.size() + 1]);
    return hi < 0 || lo < 0 ? -1 : hi << 4 | lo;
}

constexpr std::string_view kKeyAreaKeyPrefixes[] = {"key_area_key_application_", "key_area_key_ocean_",
                                                    "key_area_key_system_"};

}  // namespace

absl::StatusOr<KeySet> KeySet::Parse(std::string_view text) {
    KeySet keys;
    int line_number = 0;
    for (std::string_view line : absl::StrSplit(text, '\n')) {
        ++line_number;
        if (const std::size_t comment = line.find_first_of(";#"); comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }
        line = absl::StripAsciiWhitespace(line);
        if (line.empty()) continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return absl::InvalidArgumentError(
                absl::StrFormat("prod.keys line %d is not \"name = value\"", line_number));
        }
        const std::string name = absl::AsciiStrToLower(absl::StripAsciiWhitespace(line.substr(0, equals)));
        const std::string_view value = absl::StripAsciiWhitespace(line.substr(equals + 1));
        auto decode = [&](std::span<std::uint8_t> out) -> absl::Status {
            if (DecodeHex(value, out)) return absl::OkStatus();
            return absl::InvalidArgumentError(
                absl::StrFormat("prod.keys line %d: %s is not %d hex digits", line_number, name, out.size() * 2));
        };
        if (name == "header_key") {
            AesXtsKey key;
            if (absl::Status s = decode(key); !s.ok()) return s;
            keys.header_key_ = key;
            continue;
        }
        if (const int revision = Revision(name, "titlekek_"); revision >= 0 && revision < kRevisions) {
            Aes128Key key;
            if (absl::Status s = decode(key); !s.ok()) return s;
            keys.titlekeks_[revision] = key;
            continue;
        }
        for (std::size_t kind = 0; kind < std::size(kKeyAreaKeyPrefixes); ++kind) {
            if (const int revision = Revision(name, kKeyAreaKeyPrefixes[kind]);
                revision >= 0 && revision < kRevisions) {
                Aes128Key key;
                if (absl::Status s = decode(key); !s.ok()) return s;
                keys.key_area_keys_[kind][revision] = key;
            }
        }
    }
    return keys;
}

absl::StatusOr<KeySet> KeySet::Load(const std::string& path) {
    absl::StatusOr<StoragePtr> file = OpenFileStorage(path);
    if (!file.ok()) return file.status();
    constexpr std::uint64_t kMaxSize = 1 << 20;
    if ((*file)->size() > kMaxSize) {
        return absl::InvalidArgumentError(absl::StrFormat("%s is too large for a prod.keys", path));
    }
    absl::StatusOr<std::vector<std::uint8_t>> bytes = ReadBytes(**file, 0, (*file)->size());
    if (!bytes.ok()) return bytes.status();
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    absl::StatusOr<KeySet> keys = Parse(text);
    if (!keys.ok()) return absl::Status(keys.status().code(), absl::StrFormat("%s: %s", path, keys.status().message()));
    return keys;
}

const AesXtsKey* KeySet::header_key() const { return header_key_ ? &*header_key_ : nullptr; }

const Aes128Key* KeySet::key_area_key(KeyAreaKeyKind kind, int revision) const {
    const auto k = static_cast<std::size_t>(kind);
    if (k >= key_area_keys_.size() || revision < 0 || revision >= kRevisions) return nullptr;
    const std::optional<Aes128Key>& key = key_area_keys_[k][revision];
    return key ? &*key : nullptr;
}

const Aes128Key* KeySet::titlekek(int revision) const {
    if (revision < 0 || revision >= kRevisions) return nullptr;
    const std::optional<Aes128Key>& key = titlekeks_[revision];
    return key ? &*key : nullptr;
}

void KeySet::AddTitleKey(const RightsId& rights_id, const Aes128Key& encrypted_title_key) {
    title_keys_[rights_id] = encrypted_title_key;
}

const Aes128Key* KeySet::encrypted_title_key(const RightsId& rights_id) const {
    const auto it = title_keys_.find(rights_id);
    return it == title_keys_.end() ? nullptr : &it->second;
}

std::string KeySet::KeyAreaKeyName(KeyAreaKeyKind kind, int revision) {
    const auto k = static_cast<std::size_t>(kind);
    if (k >= std::size(kKeyAreaKeyPrefixes)) return absl::StrFormat("key_area_key_%d_%02x", k, revision);
    return absl::StrFormat("%s%02x", kKeyAreaKeyPrefixes[k], revision);
}

std::string KeySet::TitlekekName(int revision) { return absl::StrFormat("titlekek_%02x", revision); }

}  // namespace nxmac::fs
