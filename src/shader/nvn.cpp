#include "nvn.h"

#include <set>
#include <span>

#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "nxmac/fs/crypto.h"

namespace nxmac::shader::internal {

std::string SphStage(Bytes code) {
    if (Len(code) < 0x34) return "";  // shorter than the header and SPH word 0
    const std::uint32_t w = U32(code, 0x30);
    // Catherine: Full Body's GLSLC writes 0 for the type of its vertex shaders (word 0 = 0x0061), as Ryujinx reads it
    if (((w >> 10) & 0xf) == 0 && (w & 0x1f) == 1) return "vs";
    switch ((w >> 10) & 0xf) {
        case 1:
            return "vs";
        case 2:
            return "hs";
        case 3:
            return "ds";
        case 4:
            return "gs";
        case 5:
            return "ps";
        default:
            return "";
    }
}

std::string StageName(const std::string& st) {
    static const std::map<std::string, std::string> names = {{"vs", "vertex"},   {"hs", "hull"},  {"ds", "domain"},
                                                             {"gs", "geometry"}, {"ps", "pixel"}, {"cs", "compute"},
                                                             {"ts", "task"},     {"ms", "mesh"}};
    const auto it = names.find(st);
    return it == names.end() ? st : it->second;
}

bool Nvn2Ctl(Bytes ctl) {
    return Len(ctl) >= kNvn2KeyCtl && ctl.substr(0, 4) == kCtlMagic && ctl.substr(4, 4) == Lit("\xc9\0\0\0");
}

std::int64_t CtlLen(Bytes ctl) {
    if (Nvn2Ctl(ctl)) return kNvn2KeyCtl;
    const std::uint32_t n = U32(ctl, 0x20);
    return n >= 0x760 && n < 0x880 ? n : 0x880;
}

std::string KeyCtl(Bytes ctl, const KeySkip& skip) {
    std::string c(Head(ctl, CtlLen(ctl)));
    if (Nvn2Ctl(ctl)) {
        // +0x30 holds the section's own length in BNSH files and 0 elsewhere: the key takes it as 0 (the host's
        // keys_hash)
        return absl::StrCat(Head(c, 0x30), std::string(4, '\0'), From(c, 0x34));
    }
    if (skip.length && skip.offset + skip.length <= Len(c)) {
        for (std::int64_t i = 0; i < skip.length; ++i) c[static_cast<std::size_t>(skip.offset + i)] = '\0';
    }
    return c;
}

std::string Skey(Bytes ctl, Bytes code, const KeySkip& skip) {
    const std::string bytes = absl::StrCat(KeyCtl(ctl, skip), code);
    const fs::Sha256Hash hash =
        fs::Sha256(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    return absl::BytesToHexString(Bytes(reinterpret_cast<const char*>(hash.data()), 8));
}

bool SameShader(Bytes ca, Bytes ka, Bytes cb, Bytes kb, const KeySkip& skip) {
    PyInt n = Len(ca) >= 0x24 ? static_cast<PyInt>(U32(ca, 0x20)) : Len(ca);
    n = (n >= 0x760 && n <= std::min(Len(ca), Len(cb))) ? n : std::max(Len(ca), Len(cb));
    if (Nvn2Ctl(ca)) return ka == kb && KeyCtl(ca, skip) == KeyCtl(cb, skip);
    std::string a, b;
    if (skip.length && skip.offset + skip.length <= n) {
        // the bytes the key leaves out may differ
        a = absl::StrCat(Head(ca, skip.offset), Slice(ca, skip.offset + skip.length, n));
        b = absl::StrCat(Head(cb, skip.offset), Slice(cb, skip.offset + skip.length, n));
        n -= skip.length;
        ca = a;
        cb = b;
    }
    return ka == kb && Head(ca, n) == Head(cb, n);
}

std::optional<std::string> Check(const std::string& st, Bytes ctl, Bytes code) {
    if (Head(ctl, 4) != kCtlMagic || Len(ctl) < 0x24 || Len(ctl) < CtlLen(ctl)) return "bad control section";
    if (Nvn2Ctl(ctl)) {
        if (U32(ctl, 0x44) == static_cast<std::uint64_t>(Len(code))) return std::nullopt;
        return "control +0x44 does not match the code area length";
    }
    // a compute code area may have no header (SDK 10: the instructions start at +0)
    const Bytes magic = Head(code, 4);
    if ((st == "cs" && (magic == kCsMagic || magic == kGfxMagic) && magic != kCsMagic) ||
        (st != "cs" && magic != kGfxMagic)) {
        return "code area header magic does not match the stage";
    }
    if (U32(ctl, 0x704) != static_cast<std::uint64_t>(Len(code)))
        return "control +0x704 does not match the code area length";
    return std::nullopt;
}

std::string Nvn2Stage(Bytes ctl, Bytes code) {
    if (Len(ctl) > 0x56) {
        switch (static_cast<std::uint8_t>(ctl[0x56])) {
            case 5:
                return "cs";
            case 6:
                return "ts";
            case 7:
                return "ms";
            default:
                break;
        }
    }
    if (Len(code) < 4) return "";
    return SphStage(absl::StrCat(std::string(0x30, '\0'), Head(code, 4)));
}

std::string GlslcStage(Bytes ctl, Bytes code) {
    if (Nvn2Ctl(ctl)) return Nvn2Stage(ctl, code);
    const Bytes magic = Head(code, 4);
    const bool headerless_cs = magic != kCsMagic && magic != kGfxMagic && Len(ctl) >= 0x754 && U32(ctl, 0x748) &&
                               U32(ctl, 0x74c) && U32(ctl, 0x750);
    if (magic == kCsMagic || headerless_cs) return "cs";
    if (magic == kGfxMagic && Len(code) >= 0x34) return SphStage(code);
    return "";
}

GlslcLayout GlslcLayoutAt(Bytes d, PyInt h) {
    const auto w = [&](int o) { return static_cast<PyInt>(U32(d, h + o)); };
    if (h + 0x68 <= Len(d) && w(0x30) == 0xc9 && !w(0x44)) return {w(0x5c), w(0x64), 0xa8, 0x98};
    if (h + 0x50 <= Len(d)) return {w(0x44), w(0x4c), 0x90, 0x90};
    return {0, 0, 0x90, 0x90};
}

std::optional<std::vector<std::pair<std::string, std::string>>> GlslcSection(Bytes d, PyInt h, PyInt size) {
    const GlslcLayout l = GlslcLayoutAt(d, h);
    std::vector<std::pair<std::string, std::string>> secs;
    if (!(0 < l.count && l.count <= 16) || h + l.first + l.stride * l.count > Len(d)) return std::nullopt;
    for (PyInt i = 0; i < l.count; ++i) {
        const PyInt s = h + l.first + l.stride * i;
        NeedStruct(d, s, 12);
        const PyInt ssz = U32(d, s), soff = U32(d, s + 4), typ = U32(d, s + 8);
        if (typ) continue;
        NeedStruct(d, s + 0x30, 16);
        const PyInt co = U32(d, s + 0x30), ko = U32(d, s + 0x34), kn = U32(d, s + 0x38), cn = U32(d, s + 0x3c);
        if (soff + ssz > size || co + cn > ssz || ko + kn > ssz) return std::nullopt;
        secs.emplace_back(std::string(Slice(d, h + soff + co, h + soff + co + cn)),
                          std::string(Slice(d, h + soff + ko, h + soff + ko + kn)));
    }
    return secs;
}

Found GlslcProgram(Bytes d, PyInt h, PyInt size, const std::string& label) {
    const auto secs = GlslcSection(d, h, size).value_or(std::vector<std::pair<std::string, std::string>>{});
    std::vector<std::string> sts;
    sts.reserve(secs.size());
    for (const auto& [ctl, code] : secs) sts.push_back(GlslcStage(ctl, code));
    if (secs.size() < 2) return {};
    if (std::set<std::string>(sts.begin(), sts.end()).size() < sts.size()) return {};
    for (std::size_t i = 0; i < secs.size(); ++i) {
        if (sts[i].empty() || Check(sts[i], secs[i].first, secs[i].second)) return {};
    }
    Found out;
    for (std::size_t i = 0; i < secs.size(); ++i) {
        out.push_back({absl::StrCat(label, "#GLSLC@", Hex(h), "#var"), sts[i], secs[i].first, secs[i].second});
    }
    return out;
}

Found GlslcShaders(Bytes d, const std::string& label, bool need) {
    Found out;
    FindAll(d, kGlslcMagic, [&](std::int64_t h) {
        if (h + 0x48 > Len(d)) return;
        const GlslcLayout l = GlslcLayoutAt(d, h);
        const PyInt size = l.size;
        if (l.first == 0xa8) {  // NVN2: its section headers say everything
            if (!(0 < size && size <= Len(d) - h)) return;
            const auto secs = GlslcSection(d, h, size).value_or(std::vector<std::pair<std::string, std::string>>{});
            if (secs.size() > 1) {
                Found p = GlslcProgram(d, h, size, label);
                out.insert(out.end(), p.begin(), p.end());
            } else if (!secs.empty()) {
                const std::string st = GlslcStage(secs[0].first, secs[0].second);
                if (!st.empty() && !Check(st, secs[0].first, secs[0].second)) {
                    out.push_back({absl::StrCat(label, "#GLSLC@", Hex(h)), st, secs[0].first, secs[0].second});
                }
            }
            return;
        }
        const std::int64_t c = (0x760 < size && size <= Len(d) - h) ? Find(d, kCtlMagic, h, h + size) : -1;
        if (c < 0) return;
        if (Find(d, kCtlMagic, c + 4, h + size) >= 0) {
            Found p = GlslcProgram(d, h, size, label);
            out.insert(out.end(), p.begin(), p.end());
            return;
        }
        PyInt nctl = 0, ncode = 0;  // 0: a control section the output does not hold
        if (c + 0x708 <= h + size) {
            nctl = U32(d, c + 0x20);
            ncode = U32(d, c + 0x704);
        }
        std::string ctl(Slice(d, c, c + nctl)), code(Slice(d, c + nctl, c + nctl + ncode));
        std::string st;
        if (nctl >= 0x760 && !(nctl & 3) && c + nctl + ncode <= h + size) st = GlslcStage(ctl, code);
        if (st.empty() || Check(st, ctl, code)) {
            // SDK 4's GLSLC writes control sections whose +0x20 is 12 bytes short: the section header says where both
            // are
            auto sec = GlslcSection(d, h, size);
            if (!sec || sec->size() != 1) return;
            ctl = (*sec)[0].first;
            code = (*sec)[0].second;
            st = GlslcStage(ctl, code);
            if (st.empty() || Check(st, ctl, code)) return;
        }
        out.push_back({absl::StrCat(label, "#GLSLC@", Hex(h)), st, std::move(ctl), std::move(code)});
    });
    if (out.empty() && need) Fail(absl::StrCat(label, ": no GLSLC output"));
    return out;
}

std::vector<int> PsInputs(Bytes code) {
    std::vector<int> in;
    for (int i = 0; i < 32; ++i) {
        if ((U32(code, 0x30 + 4 * (6 + (i >> 2))) >> ((i & 3) * 8)) & 0xff) in.push_back(i);
    }
    return in;
}

}  // namespace nxmac::shader::internal
