#include "nxmac/loader/hle.h"

#include <cstring>
#include <utility>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace nxmac::loader {
namespace {

// FNV-1a, as sym_hash and name_h
std::uint32_t NameHash(std::string_view s) {
    std::uint32_t h = 2166136261u;
    for (const char c : s) h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
    return h;
}

std::vector<unsigned> NewTable(std::size_t n) {
    std::size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    return std::vector<unsigned>(cap, 0);
}

}  // namespace

template <typename Entry>
std::vector<unsigned> HleResolver::Index(std::span<const Entry> entries, std::string* duplicate) {
    std::vector<unsigned> index = NewTable(entries.size());
    const std::size_t mask = index.size() - 1;
    for (unsigned i = 0; i < entries.size(); ++i) {
        for (std::size_t j = NameHash(entries[i].name) & mask;; j = (j + 1) & mask) {
            if (index[j] == 0) {
                index[j] = i + 1;
                break;
            }
            if (std::strcmp(entries[index[j] - 1].name, entries[i].name) == 0) {  // the first of a name stays
                if (duplicate != nullptr && duplicate->empty()) *duplicate = entries[i].name;
                break;
            }
        }
    }
    return index;
}

template <typename Entry>
const Entry* HleResolver::Lookup(std::span<const Entry> entries, const std::vector<unsigned>& index,
                                 std::string_view name) {
    const std::size_t mask = index.size() - 1;
    for (std::size_t j = NameHash(name) & mask; index[j] != 0; j = (j + 1) & mask) {
        const Entry& e = entries[index[j] - 1];
        if (name == e.name) return &e;
    }
    return nullptr;
}

absl::StatusOr<HleResolver> HleResolver::Create(std::span<const HleEntry> hle, std::span<const HleForceEntry> force,
                                                HleOptions options) {
    HleResolver r;
    r.hle_ = hle;
    r.force_ = force;
    r.options_ = std::move(options);
    r.hle_index_ = Index(hle, nullptr);
    std::string duplicate;
    r.force_index_ = Index(force, &duplicate);
    if (!duplicate.empty()) {
        return absl::InvalidArgumentError(absl::StrCat("duplicate entry in the forced takeover list: ", duplicate));
    }
    r.force_hits_.assign(force.size(), 0);
    r.stub_index_ = NewTable(256);  // 512 slots at first, as resolve_import
    return r;
}

std::uint64_t HleResolver::Forced(std::string_view name) {
    const HleForceEntry* const e = Lookup(force_, force_index_, name);
    if (e == nullptr) return 0;
    if (force_hits_[static_cast<std::size_t>(e - force_.data())]++ == 0 && options_.log_imports) {
        Log(absl::StrFormat("force %s  (%s)", name, e->why));
    }
    return reinterpret_cast<std::uint64_t>(e->fn);
}

bool HleResolver::Implements(std::string_view name) { return Lookup(hle_, hle_index_, name) != nullptr; }

absl::StatusOr<std::uint64_t> HleResolver::Import(std::string_view /*module*/, std::string_view name) {
    if (const HleEntry* const e = Lookup(hle_, hle_index_, name)) {
        if (options_.log_imports) {
            Log(absl::StrFormat("import %s -> %s", name, options_.describe ? options_.describe(e->fn) : "?"));
        }
        return reinterpret_cast<std::uint64_t>(e->fn);
    }
    // The stub of that name when there is one already, else the next one
    std::size_t mask = stub_index_.size() - 1;
    for (std::size_t j = NameHash(name) & mask; stub_index_[j] != 0; j = (j + 1) & mask) {
        if (stub_names_[stub_index_[j] - 1] == name) {
            return options_.stub ? options_.stub(stub_index_[j] - 1) : std::uint64_t{0};
        }
    }
    if (stub_names_.size() >= kMaxStubs) return absl::ResourceExhaustedError("stub overflow");
    const auto k = static_cast<unsigned>(stub_names_.size());
    stub_names_.emplace_back(name);
    if (stub_names_.size() * 2 > stub_index_.size()) {  // past half full: rebuilt at twice the size
        stub_index_.assign(stub_index_.size() * 2, 0);
        mask = stub_index_.size() - 1;
        for (unsigned i = 0; i < stub_names_.size(); ++i) {
            std::size_t j = NameHash(stub_names_[i]) & mask;
            while (stub_index_[j] != 0) j = (j + 1) & mask;
            stub_index_[j] = i + 1;
        }
    } else {
        std::size_t j = NameHash(name) & mask;
        while (stub_index_[j] != 0) j = (j + 1) & mask;
        stub_index_[j] = k + 1;
    }
    return options_.stub ? options_.stub(k) : std::uint64_t{0};
}

absl::Status HleResolver::CheckForced(bool strict) {
    unsigned unused = 0;
    for (std::size_t i = 0; i < force_.size(); ++i) {
        if (force_hits_[i] != 0) continue;
        if (strict || options_.log_imports) {
            Log(absl::StrCat("forced takeover entry with no reference at all: ", force_[i].name));
        }
        ++unused;
    }
    if (unused != 0 && strict) return absl::InvalidArgumentError("forced takeover list is wrong");
    Log(absl::StrFormat("force: %u entries hit, %u not referenced by this game", force_.size() - unused, unused));
    return absl::OkStatus();
}

}  // namespace nxmac::loader
