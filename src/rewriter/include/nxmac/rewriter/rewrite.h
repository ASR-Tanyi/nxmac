// The NSO rewriter: a Switch aarch64 module becomes an image that runs natively on Apple systems, where the kernel
// changes x18 and tpidr_el0 and Horizon's thread area, svc and timers have no counterpart. A port of nxnative's
// dm/tools/patch_nso.py, written to give the same bytes: the host that loads the result is nxnative's.
//
//   x18 read / write        -> a thread slot (TSD, through tpidrro_el0)
//   mrs / msr tpidr_el0, mrs tpidrro_el0 -> thread slots (the guest's thread pointer and ThreadLocalRegion)
//   svc #n, mrs cntpct_el0 / cntvct_el0 -> the host gate (looked up by return point in <name>.gates.tsv)
//   sub sp, sp, xN          -> xN rounded up to 16 first (Apple's kernel faults on a misaligned sp)
//   ldxr / stxr and their kin -> ldaxr / stlxr: every exclusive access gets acquire or release ordering
//   spin wait loops         -> a copy whose back-edges pass a spin gate, where the host yields
//   registered cuts, observation gates, frame-step stubs: the per-build patch tables (patches.h)
//
// Each changed instruction becomes a branch to a stub in a patch area appended after the image (or before the base,
// or both, when the module is too large for a branch to reach). Not ported yet, and reported when a module shows the
// need: nxnative's finders for Unreal Engine, Unity and Snowdrop games (ue_*.py, unity_*.py, snowdrop_pools.py,
// fps_target.py), frame steps placed by function match, and the stubs of games other than those in src/rewriter/games.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "nxmac/rewriter/module.h"
#include "nxmac/rewriter/patches.h"

namespace nxmac::rewriter {

struct RewriteOptions {
    std::string name;                     // the module's name: main, sdk, subsdk0, ... (some rewrites are main's alone)
    std::string game;                     // the profile (base title ID) whose function patches apply; may be empty
    const PatchTables* tables = nullptr;  // the built-in ones when null
    bool list_spin_waits = false;         // a log line per spin wait found
};

// What nxnative's host reads from the module's folder, and what the rewrite found
struct RewriteResult {
    std::vector<std::uint8_t> patched;  // <name>.patched.bin
    std::string fixups;                 // <name>.fixups.tsv: the layout, the key page indices, the build ID
    std::string gates;                  // <name>.gates.tsv: return point, kind, argument, original offset
    std::string sites;                  // <name>.sites.tsv
    std::string cuts;                   // <name>.cuts.tsv
    std::string changed;                // <name>.changed.tsv: the text words that no longer run as the file has them
    std::string obs;                    // <name>.obs.tsv
    std::optional<std::string> match;  // <name>.match.tsv: an unregistered build's function patches, when there are any
    std::vector<std::string> log;      // what patch_nso.py prints; a line that starts with [RED] is a problem
    std::string summary;               // the counts of each kind of site
};

// InvalidArgument / FailedPrecondition with the reason when the module cannot be rewritten: a cut whose original
// bytes differ, a jump table through x18 it cannot read, a branch into the middle of an exclusive sequence. Nothing
// is written then: a module rewritten in part would fail in the game instead.
absl::StatusOr<RewriteResult> Rewrite(const Module& module, const RewriteOptions& options);

}  // namespace nxmac::rewriter
