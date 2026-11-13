// The host gates: the rewriter replaced every svc #n, mrs cntpct_el0 / cntvct_el0, observation point and spin-wait pass
// with a branch into the host's gate, and listed each one in <module>.gates.tsv by the return point of its gate block.
// The host looks the gate up by that address to know what the instruction was. A port of nxnative's dm/host/hle.c
// gate table (gate_register, gate_dispatch's lookup) and dm/host/loader.c register_gates.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace nxmac::loader {

enum class GateKind { kSvc, kCntpct, kCntvct, kObs, kSpin };

struct Gate {
    std::uint64_t ret = 0;  // the gate block's return point (absolute); 0: an empty slot
    GateKind kind = GateKind::kSvc;
    int arg = 0;             // svc: its number; cntpct / cntvct: the register; obs: number | parameter << 8
    std::uint64_t orig = 0;  // the original instruction's offset in its module
};

inline constexpr int kObsMax = 27;  // observation gate numbers are 1 to kObsMax - 1 (dm.h OBS_MAX)

class GateTable {
public:
    GateTable();

    // gate_register. InvalidArgument (where nxnative's host stops) for an unknown kind, an observation number out of
    // range, a return point registered twice, or more gates than a quarter of the table.
    absl::Status Register(std::uint64_t ret, std::string_view kind, int arg, std::uint64_t orig);
    // The gate whose block returns to `ret`; null when there is none (nxnative's host stops: "unknown gate")
    const Gate* Find(std::uint64_t ret) const;

    int count() const { return count_; }
    bool obs_present(int id) const { return id > 0 && id < kObsMax && obs_have_[id]; }  // gate_obs_present

    static constexpr int kBits =
        15;  // 32768 slots for at most 8192 gates (Tears of the Kingdom's main: 843 spin gates)

private:
    static unsigned Hash(std::uint64_t a);

    std::vector<Gate> slots_;
    int count_ = 0;
    bool obs_have_[kObsMax] = {};
};

// <dir>/<name>.gates.tsv into `table`: each row's return point (made absolute with the module's `base` address), kind,
// argument and original offset; rows that are not four such fields are skipped, and no file means no gates. The number
// registered.
absl::StatusOr<int> RegisterGates(GateTable& table, const std::filesystem::path& dir, std::string_view name,
                                  std::uint64_t base);

}  // namespace nxmac::loader
