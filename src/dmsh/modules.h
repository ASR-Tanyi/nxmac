// The modules of nxnative's shader build (dm/gfx/tools/dmsh_*.py, c1_guard.py, cb_extent.py), ported function by
// function (internal to nxmac::dmsh). Each takes and gives what the Python function does: the Metal source as text, a
// dict or list as Json (the reflection's bindings, the translator's res.json), a tuple as a struct. A ValueError the
// Python raises is a py::ValueError here, with the same message; a shape the port leaves out (Ampere's HMMA, the
// geometry stage's helpers) raises one that says it is not ported. Only BotW's path is ported: vertex, pixel and
// compute shaders of NVN 1 (Maxwell).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.h"
#include "py.h"

namespace nxmac::dmsh {

// ---- dmsh_atomic.py
namespace atomic {
inline constexpr std::string_view kMslVersion = "20300";
inline constexpr std::string_view kMslVersionAtomics = "30100";
// Whether a SPIR-V module has an OpImageTexelPointer (uses_image_atomics); ValueError on bytes that are not one
bool UsesImageAtomics(std::string_view spv, std::string_view path);
// xc with its --msl-version raised to 3.1 when the module has image atomics (cross_args)
std::vector<std::string> CrossArgs(const std::vector<std::string>& xc, std::string_view spv, std::string_view path);
// ValueError when SPIRV-Cross emulated an image atomic through a pointer (check_msl; the Python returns the source)
void CheckMsl(const std::string& source);
}  // namespace atomic

// ---- dmsh_wide.py
namespace wide {
std::string WideLoads(const std::string& msl);
}

// ---- dmsh_side.py
namespace side {
struct Entry {
    std::string attr, ret, name;
    std::vector<std::string> params;
    std::string decl, args;
    py::Int start = 0, head = 0, end = 0;
};
inline constexpr std::string_view kBody = "static __attribute__((always_inline)) main0_out dm_body_main0(";
// The index after the bracket closing the one opened just before i, -1 when there is none (_close)
py::Int Close(std::string_view src, py::Int i, std::string_view open, std::string_view close);
std::vector<std::string> Params(std::string_view s);
std::vector<Entry> Entries(const std::string& msl);
std::optional<Entry> FindEntry(const std::string& msl, std::string_view name);
struct BodySpan {
    py::Int start, head, end;
};
std::optional<BodySpan> Body(const std::string& msl);
std::optional<std::string> SharedBody(const std::string& msl);
std::string Calling(const Entry& e, std::string_view name, std::string_view ret, std::string_view tail);
struct Unit {
    std::optional<std::string> src;  // the compile unit, or none
    std::string why;                 // why there is none
    std::vector<std::string> alpha, logic;
};
Unit MakeUnit(const std::string& msl);
// cut(lib, *groups): the base library, then each group's; ValueError on a library it does not read
std::vector<std::string> Cut(const std::string& lib, const std::vector<std::set<std::string>>& groups);
}  // namespace side

// ---- dmsh_alpha.py
namespace alpha {
extern const std::string_view kHelper;
std::pair<std::string, std::string> Colour0(const std::string& msl, std::string_view st);  // ("", "") for none
std::optional<std::string> Refused(const std::string& msl);
std::optional<std::string> EntryText(const std::string& msl, const side::Entry& e);
}  // namespace alpha

// ---- dmsh_logic.py
namespace logic {
extern const std::string_view kHelper;
std::optional<std::string> Refused(const std::string& msl);
std::string Constants(const std::set<int>& targets);
std::optional<std::pair<std::string, std::set<int>>> EntryText(const std::string& msl, const side::Entry& e);
}  // namespace logic

// ---- dmsh_rewrite.py
namespace rewrite {
std::string Precise(const std::string& msl);
std::string MulExtended(const std::string& msl);
std::string HmmaMsl(const std::string& msl);  // ValueError for a shader with the helper: Ampere's, not ported
std::vector<std::string> SplitParams(std::string_view text);
py::Int BlockEnd(std::string_view src, py::Int start);
struct FirstStore {
    std::string name;
    py::Int word = 0, value = 0;
};
std::pair<std::string, std::vector<FirstStore>> FirstStores(const std::string& msl, const Json& local);
std::string SharedStoreMsl(const std::string& msl);
std::pair<std::string, py::Int> TgSpill(const std::string& msl);
std::optional<std::pair<std::string, std::string>> GatherQuad(const std::vector<std::string>& off);
std::string GatherOffsets(const std::string& msl);
std::string BorderEmu(const std::string& msl);
std::pair<std::string, int> FragZSnap(const std::string& msl);
std::pair<std::string, int> BlendZero(const std::string& msl);
}  // namespace rewrite

// ---- dmsh_reflect.py
namespace reflect {
struct Attribute {
    std::string type, name;
    int location = 0;
};
std::vector<Attribute> Attributes(std::string_view src);
bool InWord(std::string_view src, py::Int i);
bool HasWord(std::string_view src, std::string_view text);
Json RasterFacts(std::string_view stage, const std::string& src);
struct Tex {
    std::optional<py::Int> t, s;
    py::Int type = 0;
};
std::optional<Tex> DecodeTex(std::string_view name);
std::optional<py::Int> ConstSampler(std::string_view name);
// The sampler part: kind "id" or "c2" with a value, or a slot ("" kind) and offset
struct UboPart {
    std::string kind;      // "", "id" or "c2"
    py::Int a = 0, b = 0;  // slot, offset ("": both; "id" / "c2": b)
    bool operator==(const UboPart& o) const { return kind == o.kind && a == o.a && b == o.b; }
};
std::optional<std::pair<UboPart, UboPart>> DecodeUboTex(std::string_view name);
std::optional<std::pair<py::Int, py::Int>> DecodeUboImg(std::string_view name);
std::optional<std::pair<py::Int, std::optional<py::Int>>> C2Array(std::string_view name, std::string_view ty);
std::string PoolArrays(const std::string& src);
Json Reflect(std::string_view key, std::string_view stage, const std::string& msl, const Json& res);
Json DrvReads(const std::string& src);
Json Unfillable(const Json& dr, std::string_view stage);
}  // namespace reflect

// ---- dmsh_fetch.py
namespace fetch {
bool Once(std::string_view msl, std::string_view name);
std::optional<std::string> Proj(const std::string& msl, std::string_view var, std::string_view comp,
                                const std::map<std::string, int>& locn);
std::string SelectGuards(const std::string& msl);
Json FragPos(const std::string& msl, std::string_view var, std::string_view comp);  // null or [slot, vector, component]
std::vector<int> EntryScreen(const std::string& body, const std::map<std::string, int>& locn);
std::vector<int> ScreenVaryings(const std::string& msl);
// fold: the source with main0_f and its metadata, or the reason there is none
struct Fold {
    std::optional<std::string> src;
    Json meta;  // the metadata, or the reason (a string)
};
Fold MakeFold(const std::string& msl, const Json& bindings, bool om_only_ok);
}  // namespace fetch

// ---- dmsh_entry.py
namespace entry {
bool VsWrites(const std::string& src, const Json& binds);
std::string VsDiscard(const std::string& src);
Json IntInputs(const std::string& src);  // {location: "uint"}
std::string VsInit(const std::string& src);
std::string VsBaseInstance(const std::string& src);
std::string DropPointSize(const std::string& src);
std::string VsLayer(const std::string& src);
bool LayerOut(std::string_view src);
std::pair<std::string, Json> VsLayerEntry(const std::string& full);
std::pair<std::string, std::string> VsDepth(const std::string& src);
std::set<std::string> IntOutputs(const std::string& src, const std::string& body, const std::vector<std::string>& cols);
std::pair<std::optional<std::string>, py::Int> FsUint(const std::string& src);
std::optional<std::string> FsDual(const std::string& src);
}  // namespace entry

// ---- dmsh_vin.py, dmsh_pull.py
namespace vin {
// (the source, [{location, float, sint | uint}]) or (none, the reason)
std::pair<std::optional<std::string>, Json> Variant(const std::string& msl);
}  // namespace vin
namespace pull {
extern const std::string_view kFetch;  // dmsh_pull.metal
std::string DropEntries(const std::string& msl, const std::set<std::string>& keep);
std::pair<std::optional<std::string>, Json> Variant(const std::string& msl);
}  // namespace pull

// ---- dmsh_geometry.py: what a vertex shader's build takes from it (the capture entry point); the geometry stage
// itself is not ported
namespace geometry {
inline constexpr int kWords = 144;
inline constexpr int kCaptureSlot = 13;
std::optional<int> Helper(std::string_view name);  // HELPERS
std::string Capture(const std::string& src);
std::vector<std::pair<int, std::string>> OutputWords(const std::string& src);
}  // namespace geometry

// ---- c1_guard.py, cb_extent.py
namespace guard {
struct Num {  // a bound: a value, or none (Python's None: unbounded on that side)
    bool known = false;
    __int128 v = 0;
    Num() = default;
    Num(__int128 x) : known(true), v(x) {}  // NOLINT(google-explicit-constructor)
    explicit operator bool() const { return known; }
    __int128 operator*() const { return v; }
};
struct Bound {
    Num lo, hi;
};
using Definitions = std::map<std::string, std::pair<std::string, std::string>>;  // name: (kind, text)
Definitions DefinitionsOf(const std::string& source);
Bound Interval(const Definitions& definitions, const std::string& index);
// c1_guard.unproven for one shader: whether one of its c1 reads is not proven inside span (none: no c1)
bool Unproven(const std::string& source, std::optional<py::Int> span);
std::optional<py::Int> C1Span(const Json& refl);
// cb_extent.row's tokens for one shader
std::vector<std::string> ExtentTokens(const std::string& source, const Json& bindings);
std::map<py::Int, py::Int> Extents(const std::string& source, const Json& bindings);
std::vector<std::optional<py::Int>> Unwritten(const std::string& source,
                                              const Json& bindings);  // None: a slot not decoded
}  // namespace guard

// ---- dmsh_build.py's own SPIR-V rewrite
namespace spirv {
// spv_single_lane: the module rewritten for a subgroup of one lane; false when there is nothing to do or an operation
// it does not cover (spv untouched)
bool SingleLane(std::string& spv);
}  // namespace spirv

}  // namespace nxmac::dmsh
