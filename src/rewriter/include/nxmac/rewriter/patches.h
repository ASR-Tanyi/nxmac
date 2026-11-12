// The rewriter's per-build knowledge, as tables: nxnative keeps it in Python files (dm/tools/sdk_patches.py and
// engine_patches.py, dm/games/<id>/patches.py) and tools/export_patches.py writes their tables out as the text file
// nxmac carries (src/rewriter/patches.tsv, built into the library). What those files hold as code (the frame-step and
// stub functions of a build) is ported by hand: src/rewriter/games.
//
// One row per line, fields separated by tabs, the first naming the row:
//   source <text>                                        what the tables were made from (nxnative's commit)
//   cuts <build ID>                                      the build registers cuts (even none: it keeps its own code)
//   cut <build ID> <offset> <original> <new> <reason>    bytes as hex digits; applied before anything else
//   obs <build ID> <offset> <number> <word> <reason>     an observation gate on the instruction <word> at <offset>
//   capability <build ID> <NAME>                         a main build's feature, written into its fixups
//   frame_step <build ID> <profile>                      the build has frame-step stubs (code: src/rewriter/games)
//   stubs <build ID> <profile>                           the build has other stubs (code: src/rewriter/games)
//   match <scope> <module> <one|several> <name>          a function-matched patch of <module> (main, sdk, ...): of the
//                                                        profile <scope>, or of every game when <scope> is *
//   function <size> <fingerprints, comma-separated>      a function of the last match row
//   fcut <offset in the function> <original> <new> <reason>   a cut of the last function row
//   match_frame_step <profile> <module> <name>           a frame step placed by function match (code: not ported)
//   build <profile> <version> <display version> <module>=<build ID>...   the builds a profile was brought up on
// Lines that start with # and empty lines are skipped.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"

namespace nxmac::rewriter {

struct Cut {
    std::int64_t offset = 0;
    std::string original;  // hex digits
    std::string replacement;
    std::string reason;
};

struct ObservationGate {
    std::int64_t offset = 0;
    std::uint32_t number = 0;  // OBS_* of the host's dm.h, a register in its upper bits for some
    std::string word;          // the original instruction, 8 hex digits
    std::string reason;
};

struct FunctionRule {
    std::int64_t size = 0;
    std::vector<std::string> fingerprints;
    std::vector<Cut> cuts;  // offsets from the function's start
};

// A patch placed by the functions it changes: one function, or several that change together or not at all. Rules of
// one name are one patch's reviewed implementations: exactly one of them must match.
struct MatchRule {
    std::string name;
    bool several = false;
    std::vector<FunctionRule> functions;
};

struct ProfileBuild {
    std::string version;                                       // v1114112
    std::string display;                                       // 1.9.0
    std::vector<std::pair<std::string, std::string>> modules;  // (module name, build ID)
};

class PatchTables {
public:
    static absl::StatusOr<PatchTables> Parse(std::string_view text);
    static absl::StatusOr<PatchTables> ReadFile(const std::string& path);
    // The tables built into the library (src/rewriter/patches.tsv)
    static const PatchTables& BuiltIn();

    const std::string& source() const { return source_; }

    // nullptr when the build registers none; a build may register an empty list
    const std::vector<Cut>* Cuts(std::string_view build_id) const;
    const std::vector<ObservationGate>* Obs(std::string_view build_id) const;
    const std::vector<std::string>* Capabilities(std::string_view build_id) const;
    // The profile whose patch file has the build's frame-step (or stub) code, or nullptr
    const std::string* FrameStep(std::string_view build_id) const;
    const std::string* Stubs(std::string_view build_id) const;
    // A profile's function-matched patches of a module, and those of every game's module of that name
    const std::vector<MatchRule>* ProfileMatchCuts(std::string_view profile, std::string_view module) const;
    const std::vector<MatchRule>* SharedMatchCuts(std::string_view module) const;
    // The names of the frame steps a profile places by function match in a module (their code is not ported)
    const std::vector<std::string>* MatchFrameStep(std::string_view profile, std::string_view module) const;
    const std::vector<ProfileBuild>* Builds(std::string_view profile) const;

    std::size_t build_count() const { return cuts_.size(); }

private:
    std::string source_;
    absl::flat_hash_map<std::string, std::vector<Cut>> cuts_;
    absl::flat_hash_map<std::string, std::vector<ObservationGate>> obs_;
    absl::flat_hash_map<std::string, std::vector<std::string>> capabilities_;
    absl::flat_hash_map<std::string, std::string> frame_step_;
    absl::flat_hash_map<std::string, std::string> stubs_;
    absl::flat_hash_map<std::string, std::vector<MatchRule>> match_;  // "<scope>\t<module>"
    absl::flat_hash_map<std::string, std::vector<std::string>> match_frame_step_;
    absl::flat_hash_map<std::string, std::vector<ProfileBuild>> builds_;
};

}  // namespace nxmac::rewriter
