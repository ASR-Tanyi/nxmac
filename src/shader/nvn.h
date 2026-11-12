// NVN shaders as the split finds them (internal to nxmac::shader): a control section and a code area, their stage,
// key and checks, and GLSLC's compiler outputs (shader_split.py's sph_stage, ctl_len, key_ctl, skey, same_shader,
// check, nvn2_stage, glslc_*).
//   Code area: a 0x30 header (0x12345678 graphics, 0x12345679 compute) and, for graphics, the 0x50-byte SPH, whose
//   word 0 bits 10..13 give the stage. Control section: 0x98761234, +0x20 its own length, +0x704 the code area's
//   length, +0x748 a compute shader's thread group, +0x754 its shared memory. NVN2 (Switch 2): version 0xc9 at +4, the
//   code area's length at +0x44, the stage at +0x56 for compute, task and mesh shaders. Key: the first 16 hex digits of
//   SHA-256(the control section's first ctl_len bytes, the profile's shader_key_skip range zeroed, + the code area), as
//   nxnative's host computes it.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "py.h"

namespace nxmac::shader::internal {

inline constexpr Bytes kCtlMagic = Lit("\x34\x12\x76\x98");
inline constexpr Bytes kGfxMagic = Lit("\x78\x56\x34\x12");
inline constexpr Bytes kCsMagic = Lit("\x79\x56\x34\x12");
inline constexpr Bytes kGlslcMagic = Lit("\x91\x68\x86\x19");
inline constexpr std::int64_t kNvn2KeyCtl = 0x48;

// One shader as a reader returns it: where it was found, its stage (vs hs ds gs ps cs ts ms), its bytes
struct Shader {
    std::string where;
    std::string st;
    std::string ctl;
    std::string code;
};
using Found = std::vector<Shader>;

struct KeySkip {
    std::int64_t offset = 0;
    std::int64_t length = 0;
};

// What a scan carries: the profile's key skip, the BAHS / NDSR program variations found so far (BAHS_VAR: an entry's
// label -> its variation), and where the split's printed lines go
struct Context {
    KeySkip skip;
    std::map<std::string, std::string> bahs_var;
    std::function<void(const std::string&)> print;
};

// The stage of a graphics code area by its SPH, "" when it has none
std::string SphStage(Bytes code);
std::string StageName(const std::string& st);  // shader_map.tsv column 3: vertex, pixel, ...

bool Nvn2Ctl(Bytes ctl);
std::int64_t CtlLen(Bytes ctl);
std::string KeyCtl(Bytes ctl, const KeySkip& skip);
std::string Skey(Bytes ctl, Bytes code, const KeySkip& skip);
bool SameShader(Bytes ctl_a, Bytes code_a, Bytes ctl_b, Bytes code_b, const KeySkip& skip);
// Why a control section and a code area do not belong together, or nullopt
std::optional<std::string> Check(const std::string& st, Bytes ctl, Bytes code);
std::string Nvn2Stage(Bytes ctl, Bytes code);

// GLSLC compiler outputs
std::string GlslcStage(Bytes ctl, Bytes code);
struct GlslcLayout {
    PyInt size = 0;
    PyInt count = 0;
    PyInt first = 0;
    PyInt stride = 0;
};
GlslcLayout GlslcLayoutAt(Bytes d, PyInt h);
// The GPU code sections (control, code) of the output at h, or nullopt
std::optional<std::vector<std::pair<std::string, std::string>>> GlslcSection(Bytes d, PyInt h, PyInt size);
Found GlslcProgram(Bytes d, PyInt h, PyInt size, const std::string& label);
Found GlslcShaders(Bytes d, const std::string& label, bool need);

// The generic attributes a pixel shader reads (SPH words 6..13)
std::vector<int> PsInputs(Bytes code);

}  // namespace nxmac::shader::internal
