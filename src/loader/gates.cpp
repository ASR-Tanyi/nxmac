#include "nxmac/loader/gates.h"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"

namespace nxmac::loader {
namespace {

// A number at the start of `s` as strtoull / strtol read it; nullopt when no digit converts (sscanf's %lx and %d)
std::optional<std::uint64_t> ReadUnsigned(std::string_view s, int base) {
    const std::string c(s);
    char* end = nullptr;
    const unsigned long long v = std::strtoull(c.c_str(), &end, base);
    if (end == c.c_str()) return std::nullopt;
    return static_cast<std::uint64_t>(v);
}

std::optional<int> ReadInt(std::string_view s) {
    const std::string c(s);
    char* end = nullptr;
    const long v = std::strtol(c.c_str(), &end, 10);
    if (end == c.c_str()) return std::nullopt;
    return static_cast<int>(v);
}

}  // namespace

GateTable::GateTable() : slots_(std::size_t{1} << kBits) {}

unsigned GateTable::Hash(std::uint64_t a) {
    return static_cast<unsigned>(((a >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - kBits));
}

absl::Status GateTable::Register(std::uint64_t ret, std::string_view kind, int arg, std::uint64_t orig) {
    GateKind k;
    if (kind == "svc") {
        k = GateKind::kSvc;
    } else if (kind == "cntpct") {
        k = GateKind::kCntpct;
    } else if (kind == "cntvct") {
        k = GateKind::kCntvct;
    } else if (kind == "obs") {
        k = GateKind::kObs;
    } else if (kind == "spin") {
        k = GateKind::kSpin;
    } else {
        return absl::InvalidArgumentError(absl::StrCat("unknown gate kind ", kind));
    }
    if (k == GateKind::kObs) {
        const int id = arg & 0xff;
        if (id <= 0 || id >= kObsMax) {
            return absl::InvalidArgumentError(absl::StrFormat("observation gate number out of range %d", id));
        }
        obs_have_[id] = true;
    }
    if (count_ + 1 > (1 << kBits) / 4) return absl::InvalidArgumentError("gate overflow");
    const unsigned mask = (1u << kBits) - 1;
    unsigned i = Hash(ret);
    while (slots_[i].ret != 0) {
        if (slots_[i].ret == ret) return absl::InvalidArgumentError(absl::StrFormat("gate registered twice %#x", ret));
        i = (i + 1) & mask;
    }
    slots_[i] = {ret, k, arg, orig};
    ++count_;
    return absl::OkStatus();
}

const Gate* GateTable::Find(std::uint64_t ret) const {
    const unsigned mask = (1u << kBits) - 1;
    for (unsigned i = Hash(ret);; i = (i + 1) & mask) {
        if (slots_[i].ret == ret && ret != 0) return &slots_[i];
        if (slots_[i].ret == 0) return nullptr;
    }
}

absl::StatusOr<int> RegisterGates(GateTable& table, const std::filesystem::path& dir, std::string_view name,
                                  std::uint64_t base) {
    std::ifstream f(dir / absl::StrCat(name, ".gates.tsv"));
    int n = 0;
    for (std::string line; f && std::getline(f, line);) {
        // sscanf(line, "%lx %15s %d %lx", ...) == 4
        const std::vector<std::string_view> w = absl::StrSplit(line, absl::ByAnyChar(" \t\r\v\f"), absl::SkipEmpty());
        if (w.size() < 4 || w[1].size() > 15) continue;
        const std::optional<std::uint64_t> ret = ReadUnsigned(w[0], 16);
        const std::optional<int> arg = ReadInt(w[2]);
        const std::optional<std::uint64_t> orig = ReadUnsigned(w[3], 16);
        if (!ret || !arg || !orig) continue;
        if (absl::Status s = table.Register(base + *ret, w[1], *arg, *orig); !s.ok()) {
            return s;
        }
        ++n;
    }
    return n;
}

}  // namespace nxmac::loader
