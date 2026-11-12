// The shader split: every NVN shader of a game, from its RomFS and its main, into the package and tables nxnative's
// shader build (dm/gfx/tools/dmsh_build.py) and its host read. A port of nxnative's dm/gfx/tools/shader_split.py,
// whose output it gives byte for byte:
//   pkg/<stage>_<group>.bin   one record per shader key: 'BTCDBTCD', the key's 8 bytes, at +0x28 the control and
//                             code lengths, padded to 0x38, then the code area and the control section
//   shader_map.tsv            package, index, stage, key, where the key was first found
//   shader_facts.tsv          per compute shader its thread group and shared memory, with the profile's hand-written
//                             facts merged over them
//   shader_pairs.tsv          vertex / pixel pairs of BNSH variations (and of single-stage programs, by a cover)
//   pso_pairs.tsv             the pairs of agl's BAHS program variations the first table does not hold
// What it reads: BNSH files, agl's BAHS packs, GLSLC outputs and the other forms the split's generic scan takes, in
// files, Yaz0, SARC and gzip, and in main. Other engines' archives (Unity, Unreal, zstd, Oodle, ...) are not ported:
// such a file is left out with a [RED] line, as an unreadable one is, and an archive the split would take apart as a
// whole (a zip, a CPK, an Unreal pak, ...) stops it.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "nxmac/shader/profiles.h"

namespace nxmac::shader {

// One version of the game: its RomFS (a folder or a DMROMFS1 image) and its ExeFS main
struct SplitVersion {
    std::string romfs;
    std::string main;
};

struct SplitOptions {
    const ShaderProfile* profile = nullptr;
    std::vector<SplitVersion> versions;  // base game first; the output is their union
    std::vector<std::string> dlc;        // add-on content RomFS roots
    int jobs = 8;                        // files scanned at once
    // GLSLC outputs captured on the Mac (nxnative's DM_GLSLC_CAPTURE); empty: each version's glslc_capture beside
    // its RomFS
    std::string glslc_capture;
    // Each line shader_split.py prints ([RED] lines, progress: <done>/<total>, the summary); called from one thread at
    // a time
    std::function<void(const std::string&)> print;
};

struct SplitResult {
    std::map<std::string, std::string> package;  // pkg/ file name -> its bytes
    std::string shader_map;
    std::string shader_facts;
    std::string shader_pairs;
    std::string pso_pairs;
    std::size_t shaders = 0;
    std::size_t unreadable = 0;  // files left out
};

// A [RED] stop of the split is FailedPrecondition with its message; an unported archive Unimplemented
absl::StatusOr<SplitResult> Split(const SplitOptions& options);

// pkg/ (its old .bin files removed) and the four tables into dir
absl::Status WriteSplit(const SplitResult& result, const std::string& dir);

}  // namespace nxmac::shader
