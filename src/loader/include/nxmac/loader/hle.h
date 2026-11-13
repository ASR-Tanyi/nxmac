// The host's side of the modules' imports: its takeover tables and named stubs. HLE entries implement imports no
// module defines (the libc passthrough, the hooks); HLE_FORCE entries take a name over even where a module defines it
// (the SDK's service clients), and every entry must say why. An import nothing defines or implements gets a named stub,
// the k-th of the host's stub table, which logs the name when the game calls it. A port of nxnative's dm/host/hle.c
// (hle_find, resolve_import, hle_implements, stubs_report) and dm/host/loader.c (force_init, force_lookup,
// force_check). Not ported: DM_HLECOUNT's counting wrappers.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "nxmac/loader/relocate.h"

namespace nxmac::loader {

struct HleEntry {  // hle.h hle_ent: the __DATA,swhle section's entries
    const char* name;
    const void* fn;
};

struct HleForceEntry {  // hle.h hle_force_ent: the __DATA,swforce section's entries
    const char* name;
    const void* fn;
    const char* why;
};

inline constexpr unsigned kMaxStubs = 4096;  // the stub table's size (entry.S's .rept)

struct HleOptions {
    bool log_imports = false;                          // DM_IMPORTS: the first hit of each takeover, and each import
    std::function<void(std::string_view)> log;         // a log line (nxhost's Log)
    std::function<std::string(const void*)> describe;  // the name of a host function for the import lines (dladdr)
    std::function<std::uint64_t(unsigned)> stub;       // the address of the k-th named stub
};

class HleResolver final : public ImportResolver {
public:
    // InvalidArgument when a name is twice in the takeover table (force_init: a wrong list must fail at once). A name
    // twice in `hle` is fine: the first entry wins.
    static absl::StatusOr<HleResolver> Create(std::span<const HleEntry> hle, std::span<const HleForceEntry> force,
                                              HleOptions options);

    std::uint64_t Forced(std::string_view name) override;
    absl::StatusOr<std::uint64_t> Import(std::string_view module, std::string_view name) override;
    bool Implements(std::string_view name) override;

    // force_check, after relocation: entries no module referenced are noted (all of them with `strict` or
    // log_imports), then the line "force: N entries hit, M not referenced by this game". With `strict`
    // (DM_FORCE_STRICT) an unreferenced entry is an error: the list is wrong.
    absl::Status CheckForced(bool strict);

    unsigned stubs() const { return static_cast<unsigned>(stub_names_.size()); }  // stubs_report's count
    std::string_view stub_name(unsigned k) const { return stub_names_[k]; }       // what unimpl_c logs

private:
    HleResolver() = default;
    void Log(std::string_view line) const {
        if (options_.log) options_.log(line);
    }
    // An index into `entries` by name (open addressing, 0 = empty, else index + 1), the first of a name kept
    template <typename Entry>
    static std::vector<unsigned> Index(std::span<const Entry> entries, std::string* duplicate);
    template <typename Entry>
    static const Entry* Lookup(std::span<const Entry> entries, const std::vector<unsigned>& index,
                               std::string_view name);

    std::span<const HleEntry> hle_;
    std::span<const HleForceEntry> force_;
    std::vector<unsigned> hle_index_;
    std::vector<unsigned> force_index_;
    std::vector<unsigned> force_hits_;
    std::vector<std::string> stub_names_;
    std::vector<unsigned> stub_index_;
    HleOptions options_;
};

}  // namespace nxmac::loader
