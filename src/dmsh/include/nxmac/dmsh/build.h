// nxmac::dmsh: a game's shader library for nxnative's host, from the package nxmac's shader split (or nxnative's
// shader_split.py) wrote: nxnative's dm/gfx/tools/dmsh_build.py, rewritten. Each Switch shader goes through nxnative's
// translator (dmsh_xlate, run as a program) to SPIR-V, through SPIRV-Cross (linked in) to Metal source, through the
// rewrites, entry points and reflection of dmsh_build.py's modules, and through Apple's Metal compiler to a library;
// the result is the image b-<chain>.img holding every shader's <key>_b.metal, <key>_b.json and its libraries, the
// stamp inputs.tsv, and the tables c1_full.tsv and cb_extent.tsv beside it. What it writes is meant to be what
// dmsh_build.py writes, byte for byte, but for what nxmac's Metal convention changes (the libraries' minimum macOS).
//
// Ported: Maxwell (NVN 1) vertex, pixel and compute shaders, as Breath of the Wild has. A shader of another stage
// (geometry, tessellation), an NVN 2 (Switch 2) shader and the geo_mesh.tsv mesh libraries are reported as not ported.
#pragma once

#include <functional>
#include <string>

#include "absl/status/statusor.h"

namespace nxmac::dmsh {

struct BuildOptions {
    std::string packages;  // the split's package folder (pkg/, one <stage>_<group>.bin per stage and group)
    std::string profile;   // the game's profile folder: game.tsv and the shader tables (nxnative's dm/games/<id>)
    std::string out;       // where the library goes: <out>/b-<chain>.img
    std::string tables;    // where c1_full.tsv and cb_extent.tsv go ("": out)
    std::string xlate;     // nxnative's translator (dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate)
    std::string cache;     // the compiled shaders kept for the next build ("": <out>/cache)
    std::string min_macos = "15.0";                 // the libraries' minimum macOS; "" compiles as nxnative does
    int jobs = 0;                                   // shaders at once (0: one per CPU)
    std::function<void(const std::string&)> print;  // each line the build prints (progress, [RED] lines, the summary)
};

struct BuildResult {
    int shaders = 0, ok = 0;
    std::string image;  // the library image written
};

// FailedPrecondition and the like for a stop (a [RED] line of dmsh_build.py's that ends it, a tool that does not run);
// a shader that does not build is reported and left out, not a stop.
absl::StatusOr<BuildResult> Build(const BuildOptions& options);

}  // namespace nxmac::dmsh
