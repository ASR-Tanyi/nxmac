// The shader readers of the split (internal to nxmac::shader), each a port of the shader_split.py function of the
// same name: BNSH files, agl's BAHS packs and their program variations, and the other forms the split's generic scan
// takes in any file or in main (sead's primitive drawer, NDSR / NDVE packs, MSHD, Needle, Star Fox's shader binaries,
// PlatinumGames' shader objects and DAT archives, YEBIS's .tfxo, MonoGame's MGFX effects, Sumo's containers, Phyre's
// shaders, RAGE's effects, control sections loose in main), and the probes that tell other engines' files apart.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "nvn.h"

namespace nxmac::shader::internal {

Found BnshShaders(Context& cx, Bytes d, PyInt base, const std::string& label);
Found BahsShaders(Context& cx, Bytes d, const std::string& label);
Found SeadPrimShaders(Bytes d, const std::string& label);
Found NdsrShaders(Context& cx, Bytes d, const std::string& label);
Found NdveShaders(Context& cx, Bytes d, const std::string& label);
Found MshdShaders(Bytes d, const std::string& label);
Found NeedleShaders(Bytes d, const std::string& label);
std::optional<Found> ViperShader(Bytes d, const std::string& label);
Found PgShader(Bytes d, const std::string& label);
Found PgSizedShader(Bytes d, const std::string& label);
std::optional<std::vector<std::pair<std::string, Bytes>>> PgdatMembers(Bytes d);
Found TfxoShaders(Bytes d, const std::string& label);
Found TfxoStreams(Bytes d, const std::string& label);
Found MgfxShaders(Bytes d, const std::string& label);
std::vector<std::pair<std::string, std::string>> SumoMembers(Bytes d, const std::string& label);
Found SumoShaders(Bytes d, const std::string& label);
Found PhyreShaders(Bytes d, const std::string& label);
Found RgxdShaders(Context& cx, Bytes d, const std::string& label);
// known: the control sections the other readers took from this main, each at its own length
Found RawShaders(Context& cx, Bytes d, const std::string& label, const std::set<std::string, std::less<>>& known);
// Retro's startup material archive in main: its SNVN reader is not ported (NotPorted when main holds one)
void RetroMainForms(Bytes d, const std::string& label);
// A standalone SLCT at base: NotPorted when it is one (Monolith's HCPS reader is not ported)
void SlctShaders(Bytes d, PyInt base, const std::string& label);

// Probes of other engines' files, as the split's dispatch tests them
bool HeapsPak(Bytes file);      // heaps_pak_files(f) is not None
bool UnitySerialized(Bytes d);  // unityfs.is_serialized
bool Ue3Package(Bytes head);    // ue3_package

// main's dynamic symbols (nso_syms.dynsyms): (name, defined)
std::vector<std::pair<std::string, bool>> DynSyms(Bytes img);
// How many complete GLSL sources main embeds (profile_guess.glsl_sources)
int GlslSources(Bytes img);

// Pairs within one program variation: (vertex or mesh key, pixel key, from a BAHS program)
struct VarPair {
    std::string a;
    std::string b;
    bool prog = false;
    auto operator<=>(const VarPair&) const = default;
};
std::set<VarPair> VarPairs(const Context& cx, const Found& found);

}  // namespace nxmac::shader::internal
