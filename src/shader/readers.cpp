#include "readers.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "archive.h"

namespace nxmac::shader::internal {
namespace {

constexpr const char* kStages[] = {"vs", "hs", "ds", "gs", "ps", "cs"};  // BNSH's stage slots, NVNsh's stage numbers

// field(): an unpack of a field a header or table points at, red() naming where when it is not inside d
void Field(Bytes d, PyInt at, PyInt size, const std::string& where) {
    if (!(0 <= at && at <= Len(d) - size)) {
        Fail(absl::StrCat(where, ": ", Hex(size), " bytes at ", Hex(at), " lie outside the ", Hex(Len(d)),
                          " bytes there are"));
    }
}

// span(): d[at:at + n], red() naming where when the range is not inside d
Bytes Span(Bytes d, PyInt at, PyInt n, const std::string& where) {
    if (!(0 <= at && 0 <= n && n <= Len(d) - at)) {
        Fail(absl::StrCat(where, ": ", Hex(n), " bytes at ", Hex(at), " lie outside the ", Hex(Len(d)),
                          " bytes there are"));
    }
    return Slice(d, at, at + n);
}

std::string Repr(const std::string& st) { return st.empty() ? "None" : st; }

void Append(Found& out, Found more) {
    out.insert(out.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
}

Shader Make(std::string where, std::string st, Bytes ctl, Bytes code) {
    return {std::move(where), std::move(st), std::string(ctl), std::string(code)};
}

}  // namespace

Found BnshShaders(Context& /*cx*/, Bytes d, PyInt base, const std::string& label) {
    const std::string where = absl::StrCat(label, " BNSH @", Hex(base));
    if (Slice(d, base + 0x60, base + 0x64) != "grsc") Fail(absl::StrCat(where, " has no grsc block"));
    Field(d, base + 0x7c, 12, absl::StrCat(where, " grsc"));
    const PyInt nvar = U32(d, base + 0x7c), pvar = U64(d, base + 0x80);
    Found out;
    for (PyInt v = 0; v < nvar; ++v) {
        const PyInt at = base + pvar + 0x40 * v + 0x10;
        Field(d, at, 8, absl::StrCat(where, " variation ", Dec(v)));
        const PyInt prog = U64(d, at);
        if (!prog) continue;
        for (int si = 0; si < 6; ++si) {
            const std::string st = kStages[si];
            const PyInt sat = base + prog + 8 + 8 * si;
            Field(d, sat, 8, absl::StrCat(where, " variation ", Dec(v), " program"));
            const PyInt sc = U64(d, sat);
            if (!sc) continue;
            Field(d, base + sc + 8, 24, absl::StrCat(where, " variation ", Dec(v), " ", st));
            const PyInt pctl = U64(d, base + sc + 8), pcode = U64(d, base + sc + 16), ncode = U32(d, base + sc + 24),
                        nctl = U32(d, base + sc + 28);
            const Bytes ctl =
                Span(d, base + pctl, nctl, absl::StrCat(where, " variation ", Dec(v), " ", st, " control section"));
            const Bytes code =
                Span(d, base + pcode, ncode, absl::StrCat(where, " variation ", Dec(v), " ", st, " code area"));
            if (const auto why = Check(st, ctl, code)) {
                Fail(absl::StrCat(label, " BNSH @", Hex(base), " variation ", Dec(v), " ", st, ": ", *why));
            }
            out.push_back(Make(absl::StrCat(label, "#BNSH@", Hex(base), "#var", Dec(v)), st, ctl, code));
        }
        // NVN2's task and mesh stages follow at +0x40 and +0x48 of the program: only a pointer to a control section of
        // that stage counts (an older BNSH's program ends before them)
        for (const auto& [off, st, stage] : {std::tuple<int, const char*, int>{0x40, "ts", 6}, {0x48, "ms", 7}}) {
            if (base + prog + off + 8 > Len(d)) continue;
            const PyInt sc = U64(d, base + prog + off);
            if (!sc || base + sc + 0x20 > Len(d)) continue;
            const PyInt pctl = U64(d, base + sc + 8), pcode = U64(d, base + sc + 16), ncode = U32(d, base + sc + 24),
                        nctl = U32(d, base + sc + 28);
            if (!(0x48 <= nctl && nctl < 0x10000) || base + pctl + nctl > Len(d) || base + pcode + ncode > Len(d))
                continue;
            const Bytes ctl = Slice(d, base + pctl, base + pctl + nctl),
                        code = Slice(d, base + pcode, base + pcode + ncode);
            if (!Nvn2Ctl(ctl) || Len(ctl) <= 0x56 || static_cast<std::uint8_t>(ctl[0x56]) != stage) continue;
            if (const auto why = Check(st, ctl, code)) {
                Fail(absl::StrCat(label, " BNSH @", Hex(base), " variation ", Dec(v), " ", st, ": ", *why));
            }
            out.push_back(Make(absl::StrCat(label, "#BNSH@", Hex(base), "#var", Dec(v)), st, ctl, code));
        }
    }
    return out;
}

namespace {

// The variations of an agl BAHS file's programs (bahs_variations): its binary array, then its program array; the
// n-th vertex binary and the n-th fragment binary of a program are one variation
void BahsVariations(Context& cx, Bytes d, const std::string& label, const std::vector<PyInt>& entries) {
    if (entries.empty()) return;
    const std::set<PyInt> taken(entries.begin(), entries.end());
    const PyInt arr = *taken.begin() - 0x18;
    if (arr < 0x20) return;
    const PyInt asz = U32(d, arr), acnt = U32(d, arr + 4);
    const PyInt ntaken = static_cast<PyInt>(taken.size());
    const PyInt slack = std::max<PyInt>(1, ntaken / 100);
    if (arr + asz + 8 > Len(d) || acnt < ntaken - slack) return;
    PyInt o = arr + 8;
    std::vector<std::pair<PyInt, std::uint32_t>> recs;
    for (PyInt i = 0; i < acnt; ++i) {
        if (o + 0x20 > arr + asz) return;
        const PyInt rs = U32(d, o);
        const std::uint32_t kind = U32(d, o + 4);
        if (rs < 0x20) return;
        recs.emplace_back(o + 0x10, kind);
        o += rs;
    }
    // every entry bahs_shaders took is a record's, but for a stray match of its scan: up to 1 % may lie outside
    if (o != arr + asz) return;
    std::set<PyInt> in_recs;
    for (const auto& r : recs) in_recs.insert(r.first);
    PyInt outside = 0;
    for (const PyInt a : taken) outside += in_recs.count(a) ? 0 : 1;
    if (outside > slack) return;
    const PyInt psz = U32(d, o), pcnt = U32(d, o + 4);
    if (o + psz > Len(d) || !pcnt) return;
    PyInt q = o + 8;
    std::vector<PyInt> bases;
    for (PyInt p = 0; p < pcnt; ++p) {
        if (q + 16 > o + psz) return;
        const PyInt rs = U32(d, q), base = U32(d, q + 12);
        if (rs < 16 || base > acnt || (!bases.empty() && base < bases.back())) return;
        bases.push_back(base);
        q += rs;
    }
    if (q != o + psz) return;
    bases.push_back(acnt);
    for (PyInt p = 0; p < pcnt; ++p) {
        std::int64_t seen[2] = {0, 0};
        for (PyInt i = bases[static_cast<std::size_t>(p)]; i < bases[static_cast<std::size_t>(p + 1)]; ++i) {
            const auto& [a, k] = recs[static_cast<std::size_t>(i)];
            if (k > 1) continue;  // compute and the other kinds pair with nothing
            if (taken.count(a)) {
                cx.bahs_var[absl::StrCat(label, "#BAHS@", Hex(a))] =
                    absl::StrCat(label, "#BAHS#prog", Dec(p), "#var", seen[k]);
            }
            ++seen[k];
        }
    }
}

}  // namespace

Found BahsShaders(Context& cx, Bytes d, const std::string& label) {
    Field(d, 0x18, 8, absl::StrCat(label, " BAHS header"));
    const PyInt rsz = U32(d, 0x18), region = U32(d, 0x1c);
    if (region + rsz > Len(d)) Fail(absl::StrCat(label, " BAHS code region out of bounds"));
    struct Entry {
        PyInt a;
        PyInt c;
        Shader s;
    };
    std::vector<Entry> found;
    for (PyInt a = 0; a < region - 16; a += 4) {
        const PyInt off = U32(d, a), z = U32(d, a + 4), csz = U32(d, a + 8), rel = I32(d, a + 12);
        const bool nvn2 = 0x48 <= csz && csz < 0x10000 && 0 <= a + rel && a + rel + csz <= region &&
                          Nvn2Ctl(Slice(d, a + rel, a + rel + csz));
        // control length: 0x880 / 0x88c, 3D World's BAHS version 9 0x848; NVN2 0xd8
        if (z || !(nvn2 || csz == 0x880 || csz == 0x88c || (0x760 <= csz && csz < 0x880)) || off % 0x100) continue;
        const PyInt c = a + rel;
        if (c < 0 || c + csz > region || Slice(d, c, c + 4) != kCtlMagic) continue;
        const PyInt n = U32(d, c + (nvn2 ? 0x44 : 0x704));
        if (off + n > rsz) continue;
        const Bytes code = Slice(d, region + off, region + off + n), ctl = Slice(d, c, c + csz);
        std::string st;
        if (nvn2) {
            st = Nvn2Stage(ctl, code);
        } else if (Head(code, 4) == kCsMagic) {
            st = "cs";
        } else if (Head(code, 4) == kGfxMagic) {
            st = SphStage(code);
        }
        if (st.empty() || Check(st, ctl, code)) continue;
        found.push_back({a, c, Make(absl::StrCat(label, "#BAHS@", Hex(a)), st, ctl, code)});
    }
    // A program entry never lies inside a control section (each counted at its own length, ctl_len)
    std::vector<std::pair<PyInt, PyInt>> spans;
    spans.reserve(found.size());
    for (const Entry& e : found) spans.emplace_back(e.c, e.c + CtlLen(e.s.ctl));
    std::sort(spans.begin(), spans.end());
    std::vector<PyInt> reach(spans.size());  // the furthest end of the spans up to each
    for (std::size_t i = 0; i < spans.size(); ++i) reach[i] = std::max(spans[i].second, i ? reach[i - 1] : PyInt(0));
    const auto inside = [&](PyInt a) {
        const auto it = std::upper_bound(spans.begin(), spans.end(),
                                         std::pair<PyInt, PyInt>(a, std::numeric_limits<std::int64_t>::max()));
        if (it == spans.begin()) return false;
        return reach[static_cast<std::size_t>(it - spans.begin()) - 1] > a;
    };
    Found out;
    std::vector<PyInt> entries;
    for (Entry& e : found) {
        if (inside(e.a)) continue;
        entries.push_back(e.a);
        out.push_back(std::move(e.s));
    }
    BahsVariations(cx, d, label, entries);
    return out;
}

Found SeadPrimShaders(Bytes d, const std::string& label) {
    if (Len(d) < 0x10) return {};
    const PyInt offs[4] = {U32(d, 0), U32(d, 4), U32(d, 8), U32(d, 12)};
    Found out;
    for (const auto& [st, c, k] :
         {std::tuple<const char*, PyInt, PyInt>{"vs", offs[0], offs[2]}, {"ps", offs[1], offs[3]}}) {
        if (!(0x10 <= c && c + 0x48 <= Len(d) && Nvn2Ctl(Slice(d, c, c + 0x48)))) return {};
        const PyInt own = U32(d, c + 0x30);
        const Bytes ctl = Slice(d, c, c + std::max<PyInt>(own, 0x48));
        const PyInt n = U32(ctl, 0x44);
        const Bytes code = Slice(d, k, k + n);
        if (Len(code) != n || Nvn2Stage(ctl, code) != st || Check(st, ctl, code)) return {};
        out.push_back(Make(absl::StrCat(label, "#SEADPRIM@", Hex(c)), st, ctl, code));
    }
    return out;
}

Found NdsrShaders(Context& cx, Bytes d, const std::string& label) {
    if (Head(d, 4) != "NDSR" || Len(d) < 0x20) return {};
    const auto records_from = [&](PyInt o) {
        const PyInt count = std::min<PyInt>(16, (Len(d) - o) / 16);
        int n = 0;
        for (PyInt i = 0; i < count; ++i) {
            const PyInt c = U32(d, o + 16 * i);
            if (c + 0x48 <= Len(d) && Nvn2Ctl(Slice(d, c, c + 0x48))) ++n;
        }
        return n;
    };
    // the records start at +0x10, or at +0x14 (Splatoon Raiders' pack): where the first ones point at control sections
    PyInt o = records_from(0x14) > records_from(0x10) ? 0x14 : 0x10;
    Found out;
    std::optional<PyInt> prog;
    std::string prog_where;
    while (o + 16 <= Len(d)) {
        const PyInt c = U32(d, o), cl = U32(d, o + 4), k = U32(d, o + 8), last = U32(d, o + 12);
        if (!(c || cl || k || last)) break;
        if (!(c + 0x48 <= Len(d) && Nvn2Ctl(Slice(d, c, c + 0x48)))) {
            o += 8;
            continue;
        }
        o += 16;
        const Bytes ctl = Slice(d, c, c + std::max<PyInt>(cl >> 4, 0x48));
        const Bytes code = Slice(d, k, k + U32(ctl, 0x44));
        const std::string st = Nvn2Stage(ctl, code);
        if (st.empty() || Check(st, ctl, code)) continue;
        std::string where = absl::StrCat(label, "#NDSR@", Hex(c));
        out.push_back(Make(where, st, ctl, code));
        if (st == "vs") {
            prog = c;
            prog_where = where;
        } else if (st == "ps" && prog) {
            const std::string var = absl::StrCat(label, "#NDSR#", Hex(*prog));
            cx.bahs_var[prog_where] = var;
            cx.bahs_var[where] = var;
            prog.reset();
        }
    }
    return out;
}

Found NdveShaders(Context& cx, Bytes d, const std::string& label) {
    if (Len(d) < 0x18) return {};
    const PyInt pack = 0x18 + 8 * static_cast<PyInt>(U32(d, 0x14));
    const PyInt nets = pack + U32(d, 0xc);
    Found out = NdsrShaders(cx, Slice(d, pack, nets), label);
    for (std::int64_t c = Find(d, kCtlMagic, nets); c >= 0; c = Find(d, kCtlMagic, c + 4)) {
        if (!(c & 3) && c + 0x48 <= Len(d) && Nvn2Ctl(Slice(d, c, c + 0x48))) {
            const PyInt own = U32(d, c + 0x30), n = U32(d, c + 0x44);
            const Bytes ctl = Slice(d, c, c + own), code = Slice(d, c + own, c + own + n);
            const std::string st = Nvn2Stage(ctl, code);
            if (!st.empty() && Len(code) == n && !Check(st, ctl, code)) {
                out.push_back(Make(absl::StrCat(label, "#NDVE@", Hex(c)), st, ctl, code));
            }
        }
    }
    return out;
}

Found MshdShaders(Bytes d, const std::string& label) {
    Found out;
    std::int64_t c = Find(d, kCtlMagic);
    while (c >= 0) {
        Field(d, c + 0x20, 4, absl::StrCat(label, " MSHD control section @", Hex(c)));
        const PyInt nctl = U32(d, c + 0x20);
        const std::int64_t g = Find(d, kGfxMagic, c + nctl);
        if (nctl < 0x880 || g < 0 || g - c - nctl > 0x400) {
            Fail(absl::StrCat(label, " MSHD control section @", Hex(c), ": length ", Hex(nctl),
                              ", no code area header after it"));
        }
        const PyInt ncode = U32(d, c + 0x704);
        const Bytes ctl = Slice(d, c, c + nctl);
        const Bytes code = Span(d, g, ncode, absl::StrCat(label, " MSHD @", Hex(c), " code area"));
        if (ncode < 0x34) {
            Fail(absl::StrCat(label, " MSHD @", Hex(c), ": a code area of ", Hex(ncode),
                              " bytes, shorter than its header"));
        }
        const std::string st = SphStage(code);
        if (st != (out.size() % 2 ? "ps" : "vs")) {
            Fail(absl::StrCat(label, " MSHD @", Hex(c), ": stage ", out.size(), " is ", Repr(st),
                              ", not alternating vertex / pixel"));
        }
        if (const auto why = Check(st, ctl, code)) Fail(absl::StrCat(label, " MSHD @", Hex(c), " ", st, ": ", *why));
        out.push_back(Make(absl::StrCat(label, "#MSHD#var", out.size() / 2), st, ctl, code));
        c = Find(d, kCtlMagic, g + ncode);
    }
    if (out.size() % 2) Fail(absl::StrCat(label, " MSHD: ", out.size(), " stages, not in pairs"));
    return out;
}

Found NeedleShaders(Bytes d, const std::string& label) {
    Found out;
    FindAll(d, "HNSHV002", [&](std::int64_t h) {
        if (h + 12 > Len(d)) return;
        const PyInt size = U32Be(d, h + 8);
        if (size < 0x90 + 0x760 || size > Len(d) - h)
            return;  // a literal magic string in executable code is not a container
        const PyInt end = h + size;
        std::int64_t c = Find(d, kCtlMagic, h + 12, end);
        while (c >= 0) {
            const PyInt s = c - 0x90;
            const std::string at = absl::StrCat(label, " NEEDLE @", Hex(h));
            if (s < h + 12 || c + 0x760 > end) Fail(absl::StrCat(at, ": incomplete NVN section at ", Hex(c)));
            const PyInt co = U32(d, s + 0x30), ko = U32(d, s + 0x34), kn = U32(d, s + 0x38), cn = U32(d, s + 0x3c);
            if (co != 0x90 || cn < 0x760 || cn != U32(d, c + 0x20)) {
                Fail(absl::StrCat(at, ": invalid control offset or control length at ", Hex(s)));
            }
            if (ko < co + cn || !kn || std::max(co + cn, ko + kn) > end - s) {
                Fail(absl::StrCat(at, ": NVN section at ", Hex(s), " is outside its HNSHV002 container"));
            }
            const Bytes ctl = Slice(d, c, c + cn), code = Slice(d, s + ko, s + ko + kn);
            const std::string st = GlslcStage(ctl, code);
            const std::optional<std::string> why =
                st.empty() ? std::optional<std::string>("unknown shader stage") : Check(st, ctl, code);
            if (why) Fail(absl::StrCat(at, ": NVN section at ", Hex(s), ": ", *why));
            out.push_back(Make(absl::StrCat(label, "#NEEDLE@", Hex(s)), st, ctl, code));
            c = Find(d, kCtlMagic, s + ko + kn, end);
        }
    });
    return out;
}

std::optional<Found> ViperShader(Bytes d, const std::string& label) {
    if (Len(d) < 0x18 + kNvn2KeyCtl || Head(d, 4) != kGlslcMagic) return std::nullopt;
    const PyInt ncode = U32(d, 4), nctl = U32(d, 8);
    if (0x18 + ncode + nctl != Len(d) || !ncode || !nctl) return std::nullopt;
    const Bytes code = Slice(d, 0x18, 0x18 + ncode), ctl = From(d, 0x18 + ncode);
    if (!Nvn2Ctl(ctl)) return std::nullopt;
    const std::string st = Nvn2Stage(ctl, code);
    if (st.empty()) {
        Fail(absl::StrCat(label, ": a shader binary of no known stage (control +0x56 ",
                          Len(ctl) > 0x56 ? absl::StrCat(static_cast<std::uint8_t>(ctl[0x56])) : "None", ", code ",
                          absl::BytesToHexString(Head(code, 4)), ")"));
    }
    if (const auto why = Check(st, ctl, code)) Fail(absl::StrCat(label, " (", st, "): ", *why));
    return Found{Make(absl::StrCat(label, "#VIPER"), st, ctl, code)};
}

Found PgShader(Bytes d, const std::string& label) {
    static const std::map<Bytes, std::string> stages = {{Lit("VSO\0"), "vs"}, {Lit("HSO\0"), "hs"},
                                                        {Lit("DSO\0"), "ds"}, {Lit("GSO\0"), "gs"},
                                                        {Lit("PSO\0"), "ps"}, {Lit("CSO\0"), "cs"}};
    const std::string st = stages.at(Head(d, 4));
    const PyInt nctl = U32(d, 8), octl = U32(d, 12), ncode = U32(d, 16), ocode = U32(d, 20);
    if (octl + nctl > Len(d) || ocode + ncode > Len(d))
        Fail(absl::StrCat(label, ": shader object sections out of bounds"));
    const Bytes ctl = Slice(d, octl, octl + nctl), code = Slice(d, ocode, ocode + ncode);
    std::optional<std::string> why = Check(st, ctl, code);
    if (!why && st != "cs" && SphStage(code) != st) {
        why = absl::StrCat("the SPH stage ", Repr(SphStage(code)), " does not match the object's magic");
    }
    if (why) Fail(absl::StrCat(label, " (", st, "): ", *why));
    return {Make(absl::StrCat(label, "#PGSO"), st, ctl, code)};
}

Found PgSizedShader(Bytes d, const std::string& label) {
    if (Len(d) < 12) return {};
    const PyInt prefix = U32(d, 0);
    if (prefix < 4 || (prefix & 3) || prefix + 8 > Len(d)) return {};
    const PyInt ncode = U32(d, prefix);
    const PyInt codeoff = prefix + 4, ctlsize = prefix + 4 + ncode;
    if (ncode < 0x80 || ctlsize + 4 > Len(d)) return {};
    const Bytes magic = Slice(d, codeoff, codeoff + 4);
    if (magic != kGfxMagic && magic != kCsMagic) return {};
    const PyInt nctl = U32(d, ctlsize);
    const PyInt ctloff = ctlsize + 4;
    if (nctl < 0x760 || ctloff + nctl != Len(d) || Slice(d, ctloff, ctloff + 4) != kCtlMagic) return {};
    const Bytes ctl = From(d, ctloff), code = Slice(d, codeoff, ctlsize);
    const std::string st = Head(code, 4) == kCsMagic ? "cs" : SphStage(code);
    if (st.empty()) Fail(absl::StrCat(label, ": binding-table shader has an unknown SPH stage"));
    if (const auto why = Check(st, ctl, code)) Fail(absl::StrCat(label, " (", st, "): ", *why));
    return {Make(absl::StrCat(label, "#PGSO"), st, ctl, code)};
}

std::optional<std::vector<std::pair<std::string, Bytes>>> PgdatMembers(Bytes d) {
    const PyInt n = U32(d, 4), ot = U32(d, 8), nt = U32(d, 16), st = U32(d, 20);
    std::vector<std::pair<std::string, Bytes>> out;
    if (!n) return out;
    if (std::max({ot + 4 * n, st + 4 * n, nt + 4}) > Len(d)) return std::nullopt;
    const PyInt w = U32(d, nt);
    if (nt + 4 + w * n > Len(d)) return std::nullopt;
    for (PyInt i = 0; i < n; ++i) {
        if (static_cast<PyInt>(U32(d, ot + 4 * i)) + U32(d, st + 4 * i) > Len(d)) return std::nullopt;
    }
    for (PyInt i = 0; i < n; ++i) {
        const Bytes field = Slice(d, nt + 4 + w * i, nt + 4 + w * (i + 1));
        const std::size_t z = field.find('\0');
        const PyInt o = U32(d, ot + 4 * i), s = U32(d, st + 4 * i);
        out.emplace_back(DecodeUtf8Replace(field.substr(0, z)), Slice(d, o, o + s));
    }
    return out;
}

Found TfxoShaders(Bytes d, const std::string& label) {
    Found out;
    FindAll(d, kCtlMagic, [&](std::int64_t a) {
        if (a + 0x24 > Len(d)) return;  // the magic too near the end to start a control section
        const PyInt n = U32(d, a + 0x20);
        if (!(0x760 <= n && n <= 0x880)) return;
        const Bytes ctl = Span(d, a, n, absl::StrCat(label, ": the control section at ", Hex(a)));
        const PyInt ncode = U32(ctl, 0x704);
        const Bytes code = Slice(d, a + n, a + n + ncode);
        std::string st;
        if (Head(code, 4) == kCsMagic) {
            st = "cs";
        } else if (Head(code, 4) == kGfxMagic) {
            st = SphStage(code);
        }
        if (st.empty())
            Fail(absl::StrCat(label, ": the control section at ", Hex(a), " is not followed by a code area"));
        if (const auto why = Check(st, ctl, code)) Fail(absl::StrCat(label, ": shader at ", Hex(a), ": ", *why));
        out.push_back(Make(absl::StrCat(label, "#TFXO@", Hex(a)), st, ctl, code));
    });
    return out;
}

Found TfxoStreams(Bytes d, const std::string& label) {
    static constexpr Bytes kTfxo = Lit(".tfxo file\0");
    Found out;
    for (std::size_t at = d.find('\x78'); at != Bytes::npos; at = d.find('\x78', at + 1)) {
        if (at + 1 >= d.size()) break;
        const auto second = static_cast<std::uint8_t>(d[at + 1]);
        if (second != 0x01 && second != 0x5e && second != 0x9c && second != 0xda) continue;
        const auto p = static_cast<std::int64_t>(at);
        const std::optional<Inflated> head = TryInflate(Slice(d, p, p + 4096), kTfxo.size());
        if (!head || head->out != kTfxo) continue;
        const Inflated x = Inflate(From(d, p));  // a zlib.error here stops the split, as it does shader_split.py
        Append(out, TfxoShaders(x.out, absl::StrCat(label, "@", Hex(p))));
    }
    return out;
}

namespace {

// _Rd: a little-endian reader with .NET's BinaryReader strings, its errors Python's
struct Reader {
    Bytes d;
    PyInt p;
    std::uint8_t u8() {
        const std::uint8_t v = Index(d, p);
        ++p;
        return v;
    }
    std::uint32_t u32() {
        const std::uint32_t v = U32(d, p);
        p += 4;
        return v;
    }
    Bytes raw(PyInt n) {
        p += n;
        return Slice(d, p - n, p);
    }
    Bytes str() {
        PyInt n = 0;
        int sh = 0;
        for (;;) {
            const std::uint8_t b = u8();
            n |= static_cast<PyInt>(b & 0x7f) << sh;
            sh += 7;
            if (b < 0x80) return raw(n);
            if (sh > 100) throw PyError("OverflowError", "length too large");
        }
    }
};

std::optional<Found> MgfxEffect(Bytes d, PyInt p, const std::string& label) {
    const bool v5 = Slice(d, p + 4, p + 5) == Lit("\x05");
    if (Slice(d, p + 4, p + 5) != Lit("\x08") && !v5) return std::nullopt;
    Reader r{d, p + (v5 ? 6 : 10)};
    try {
        for (int i = 0, n = r.u8(); i < n; ++i) {
            if (!v5) r.str();
            r.raw(2);
            r.raw(3 * static_cast<PyInt>(r.u8()));
        }
        if (v5 ? Slice(d, r.p + 6, r.p + 10) != kGlslcMagic : Slice(d, r.p + 10, r.p + 14) != kCtlMagic)
            return std::nullopt;
    } catch (const PyError& e) {
        if (e.type() == "IndexError") return std::nullopt;
        throw;
    }
    const std::string at = absl::StrCat(label, " MGFX @", Hex(p));
    const auto no_annotations = [&]() {
        if (r.u8()) Fail(absl::StrCat(at, ": annotations (MonoGame writes none)"));
    };
    Found out;
    for (int i = 0, count = r.u8(); i < count; ++i) {
        const std::uint8_t vs = r.u8();
        const PyInt n = r.u32();
        if (r.p + n > Len(d)) {  // a shader whose length runs past the data: a stray 'MGFX' in other content
            if (absl::EndsWith(label, ".xnb"))
                Fail(absl::StrCat(at, ": shader ", i, " claims ", Hex(n), " bytes past the end of the file"));
            return std::nullopt;
        }
        const Bytes bc = r.raw(n);
        std::string st;
        Bytes ctl, code;
        Found g;
        if (v5) {
            g = GlslcShaders(bc, absl::StrCat(at, " shader ", i), false);
            if (vs > 1 || g.size() != 1 || Head(bc, 4) != kGlslcMagic || U32(bc, 0x44) != n) {
                Fail(absl::StrCat(at, " shader ", i, ": ", Hex(n), " bytes, not one GLSLC output of one stage"));
            }
            st = g[0].st;
            ctl = g[0].ctl;
            code = g[0].code;
        } else {
            const PyInt nctl = U32(bc, 0);
            const PyInt ncode = 8 + nctl <= n ? static_cast<PyInt>(U32(bc, 4 + nctl)) : -1;
            if (vs > 1 || 8 + nctl + ncode != n) {
                Fail(absl::StrCat(at, " shader ", i, ": ", Hex(n), " bytes, not a control section and a code area"));
            }
            ctl = Slice(bc, 4, 4 + nctl);
            code = From(bc, 8 + nctl);
            if (Head(code, 4) == kGfxMagic) st = SphStage(code);
        }
        const std::optional<std::string> why =
            st == (vs ? "vs" : "ps")
                ? Check(st, ctl, code)
                : std::optional<std::string>(
                      absl::StrCat("the SPH stage is ", Repr(st), ", the effect says ", vs ? "vertex" : "pixel"));
        if (why) Fail(absl::StrCat(at, " shader ", i, ": ", *why));
        out.push_back(Make(absl::StrCat(label, "#MGFX@", Hex(p), "#", i), st, ctl, code));
        for (int k = 0, samplers = r.u8(); k < samplers; ++k) {
            r.raw(3);
            if (r.u8()) {
                if (v5) Fail(absl::StrCat(at, " shader ", i, ": a sampler state (none in the version 5 effects seen)"));
                r.raw(20);
            }
            if (!v5) r.str();
            r.u8();
        }
        r.raw(r.u8());
        if (!v5) {
            for (int k = 0, attributes = r.u8(); k < attributes; ++k) {
                r.str();
                r.raw(4);
            }
        }
    }
    int depth = 0;
    std::function<int()> params = [&]() -> int {
        if (++depth > 980) throw PyError("RecursionError", "maximum recursion depth exceeded");
        const int n = r.u8();
        for (int k = 0; k < n; ++k) {
            const Bytes type = r.raw(2);
            if (Len(type) < 2) throw PyError("IndexError", "index out of range");
            const auto typ = static_cast<std::uint8_t>(type[1]);
            r.str();
            r.str();
            no_annotations();
            const PyInt rows = r.u8(), cols = r.u8();
            const int members = params();
            if (members + params()) continue;
            if (typ == 1 || typ == 2 || typ == 3) {
                r.raw(4 * rows * cols);
            } else if (typ == 4) {
                r.str();
            }
        }
        --depth;
        return n;
    };
    params();
    for (int t = 0, techniques = r.u8(); t < techniques; ++t) {
        r.str();
        no_annotations();
        for (int k = 0, passes = r.u8(); k < passes; ++k) {
            r.str();
            no_annotations();
            const std::uint8_t v = r.u8(), f = r.u8();
            if ((v != 255 && v >= out.size()) || (f != 255 && f >= out.size())) {
                Fail(absl::StrCat(at, ": a pass names shader ", v, " / ", f, " of ", out.size()));
            }
            for (const int skip : {18, 25, 12}) {
                if (r.u8()) r.raw(skip);
            }
        }
    }
    // in Bastion's XNB files the length in front of 'MGFX' runs to the file's end, past the effect, all zeros
    bool trailing = false;
    if (absl::EndsWith(label, ".xnb")) {
        for (const char c : From(d, r.p)) trailing |= c != '\0';
    }
    if (r.p > Len(d) || trailing)
        Fail(absl::StrCat(at, ": the effect ends at ", Hex(r.p), ", the file at ", Hex(Len(d))));
    return out;
}

}  // namespace

Found MgfxShaders(Bytes d, const std::string& label) {
    Found out;
    FindAll(d, "MGFX", [&](std::int64_t p) {
        if (std::optional<Found> effect = MgfxEffect(d, p, label)) Append(out, std::move(*effect));
    });
    if (absl::EndsWith(label, ".xnb") && Contains(Head(d, 0x200), "EffectReader") && out.empty()) {
        Fail(absl::StrCat(label, ": an XNB effect without an MGFX version 5 or 8 effect of NVN shaders"));
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> SumoMembers(Bytes d, const std::string& label) {
    if (Len(d) < 44) Fail(absl::StrCat(label, ": truncated MuxFDMR header"));
    PyInt head = U32(d, 8);
    const PyInt n = static_cast<std::uint8_t>(d[18]);
    if (static_cast<std::uint8_t>(d[17]) != 2 || (n != 1 && n != 2) || head != 44 + 4 * n || head > Len(d)) {
        Fail(absl::StrCat(label, ": unsupported MuxFDMR layout"));
    }
    std::vector<PyInt> sizes;
    PyInt total = 0;
    for (PyInt i = 0; i < n; ++i) {
        sizes.push_back(U32(d, 40 + 4 * i));
        total += sizes.back();
    }
    if (head + total != Len(d)) Fail(absl::StrCat(label, ": MuxFDMR streams do not fit the file"));
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const PyInt expected = U32(d, 24 + 8 * static_cast<PyInt>(i));
        const Inflated z = Inflate(Slice(d, head, head + sizes[i]), static_cast<std::size_t>(expected) + 1);
        if (!z.eof || z.unused || static_cast<PyInt>(z.out.size()) != expected) {
            Fail(absl::StrCat(label, ": MuxFDMR stream ", i, " length does not match its header"));
        }
        out.emplace_back(absl::StrCat("#MUX", i), z.out);
        head += sizes[i];
    }
    return out;
}

namespace {

Shader SumoProgram(Bytes d, PyInt base, PyInt size, PyInt co, PyInt ko, PyInt cn, PyInt kn, const std::string& label,
                   const std::string& stage) {
    if (base < 0 || size < 0x34 || base + size > Len(d) || cn < 0x760 || kn < 0x40 || std::min(co, ko) < 0x34 ||
        std::max(co + cn, ko + kn) > size || !(co + cn <= ko || ko + kn <= co)) {
        Fail(absl::StrCat(label, ": shader ranges do not fit the container"));
    }
    const Bytes ctl = Slice(d, base + co, base + co + cn), code = Slice(d, base + ko, base + ko + kn);
    const std::string st = GlslcStage(ctl, code);
    if (st.empty() || (!stage.empty() && st != stage)) Fail(absl::StrCat(label, ": invalid shader stage"));
    if (const auto why = Check(st, ctl, code)) Fail(absl::StrCat(label, ": ", *why));
    return Make(label, st, ctl, code);
}

}  // namespace

Found SumoShaders(Bytes d, const std::string& label) {
    Found out;
    FindAll(d, Lit("Ninty_NX\0\0\0\0\0\0\0\0"), [&](std::int64_t m) {
        const PyInt r = m - 0x30;
        if (r < 0) return;
        const PyInt base = r - static_cast<PyInt>(U64(d, r + 0x28));
        if (base < 0 || r < base + 0x38) return;
        const PyInt size = U64(d, base + 8);
        if (r + 0x40 > base + size || base + size > Len(d)) return;
        if (U32(d, base + 4) != 1) Fail(absl::StrCat(label, ": unsupported Ninty_NX container version"));
        const PyInt co = U64(d, r), ko = U64(d, r + 8), cn = U32(d, r + 16), kn = U32(d, r + 20);
        out.push_back(SumoProgram(d, base, size, co, ko, cn, kn, absl::StrCat(label, "#SUMO@", Hex(r)), ""));
    });
    FindAll(d, Lit("NVNsh\0"), [&](std::int64_t h) {
        if (h + 0x34 > Len(d)) return;
        const PyInt size = U32(d, h + 8);
        if (!(0x34 <= size && size <= Len(d) - h)) return;
        if (U16(d, h + 6) != 1) Fail(absl::StrCat(label, ": unsupported NVNsh version"));
        const PyInt stage = U32(d, h + 28);
        if (stage >= 6) Fail(absl::StrCat(label, ": invalid NVNsh stage ", Dec(stage)));
        const PyInt co = U32(d, h + 32), ko = U32(d, h + 36), kn = U32(d, h + 40), cn = U32(d, h + 44);
        out.push_back(SumoProgram(d, h, size, co, ko, cn, kn, absl::StrCat(label, "#NVNSH@", Hex(h)),
                                  kStages[static_cast<std::size_t>(stage)]));
    });
    return out;
}

Found PhyreShaders(Bytes d, const std::string& label) {
    Found out;
    FindAll(d, kCtlMagic, [&](std::int64_t c) {
        if (c + 0x708 > Len(d)) return;
        const PyInt cn = U32(d, c + 0x20), kn = U32(d, c + 0x704);
        const PyInt k = c - kn;
        if (k < 0 || kn < 0x80 || cn < 0x760 || c + cn > Len(d) || Slice(d, k, k + 4) != kGfxMagic) return;
        const Bytes ctl = Slice(d, c, c + cn), code = Slice(d, k, c);
        const std::string st = SphStage(code);
        if (!st.empty() && !Check(st, ctl, code))
            out.push_back(Make(absl::StrCat(label, "#PHYRE@", Hex(k)), st, ctl, code));
    });
    return out;
}

Found RgxdShaders(Context& cx, Bytes d, const std::string& label) {
    Found out;
    const std::int64_t n = Count(d, kCtlMagic);
    FindAll(d, kGlslcMagic, [&](std::int64_t m) {
        const PyInt h = m - 4;
        if (h < 0 || h + 16 > Len(d)) return;
        const PyInt nk = U32(d, h + 8), nc = U32(d, h + 12);
        if (h + 16 + nk + nc > Len(d)) return;
        const Bytes code = Slice(d, h + 16, h + 16 + nk), ctl = Slice(d, h + 16 + nk, h + 16 + nk + nc);
        const std::string st = Head(ctl, 4) == kCtlMagic && Len(ctl) >= 0x760 ? GlslcStage(ctl, code) : "";
        if (!st.empty() && !Check(st, ctl, code))
            out.push_back(Make(absl::StrCat(label, "#RGXD@", Hex(h)), st, ctl, code));
    });
    if (static_cast<std::int64_t>(out.size()) != n) {
        cx.print(absl::StrCat("[RED] ", label, ": ", n, " control sections, ", out.size(),
                              " shader records read; the rest are left out"));
    }
    return out;
}

namespace {

// rel_targets: a module image's relative relocations {slot: target}, in the order Python's dict keeps them
struct RelTargets {
    std::vector<std::pair<std::uint64_t, PyInt>> items;
    std::unordered_map<std::uint64_t, std::size_t> index;
    void Set(std::uint64_t slot, PyInt target) {
        const auto [it, added] = index.emplace(slot, items.size());
        if (added) {
            items.emplace_back(slot, target);
        } else {
            items[it->second].second = target;
        }
    }
    const PyInt* Get(PyInt slot) const {
        if (slot < 0 || slot > static_cast<PyInt>(std::numeric_limits<std::uint64_t>::max())) return nullptr;
        const auto it = index.find(static_cast<std::uint64_t>(slot));
        return it == index.end() ? nullptr : &items[it->second].second;
    }
};

RelTargets RelTargetsOf(Bytes img) {
    const PyInt m = U32(img, 4);
    PyInt o = m + I32(img, m + 4);
    std::map<std::int64_t, std::uint64_t> t;
    while (o + 16 <= Len(img)) {
        const std::int64_t tag = I64(img, o);
        const std::uint64_t v = U64(img, o + 8);
        o += 16;
        if (tag == 0) break;
        t.emplace(tag, v);
    }
    RelTargets out;
    if (t.count(7)) {
        const PyInt size = t.count(8) ? static_cast<PyInt>(t[8]) : 0;
        for (PyInt i = 0; i < size; i += 24) {
            const PyInt at = static_cast<PyInt>(t[7]) + i;
            NeedStruct(img, at, 24);
            const std::uint64_t off = U64(img, at), info = U64(img, at + 8);
            const std::int64_t add = I64(img, at + 16);
            if ((info & 0xffffffff) == 0x403) out.Set(off, add);
        }
    }
    if (t.count(36)) {  // DT_RELR
        const PyInt size = t.count(35) ? static_cast<PyInt>(t[35]) : 0;
        PyInt nxt = 0;
        for (PyInt i = 0; i < size; i += 8) {
            const std::uint64_t e = U64(img, static_cast<PyInt>(t[36]) + i);
            if (!(e & 1)) {
                out.Set(e, U64(img, e));
                nxt = static_cast<PyInt>(e) + 8;
                continue;
            }
            for (int b = 0; b < 63; ++b) {
                if ((e >> (b + 1)) & 1) {
                    const PyInt s = nxt + 8 * b;
                    const std::uint64_t target = U64(img, s);
                    out.Set(static_cast<std::uint64_t>(s), target);
                }
            }
            nxt += 8 * 63;
        }
    }
    return out;
}

// adrp_targets: the addresses in [lo, hi) the code builds with adrp + add on one register -> the adds' offsets
std::map<PyInt, std::vector<PyInt>> AdrpTargets(Bytes d, PyInt lo, PyInt hi) {
    std::map<PyInt, std::vector<PyInt>> out;
    const std::size_t words = d.size() / 4;
    const auto w = [&](std::size_t i) { return static_cast<std::uint32_t>(U32(d, static_cast<PyInt>(i) * 4)); };
    for (std::size_t i = 0; i < words; ++i) {
        const std::uint32_t x = w(i);
        if ((x & 0x9f000000) != 0x90000000) continue;
        const std::uint32_t rd = x & 31;
        // as shader_split.py computes it: the 21-bit immediate unsigned (Python's << 44 >> 44 keeps every bit)
        const PyInt imm = static_cast<PyInt>((((x >> 5) & 0x7ffff) << 2) | ((x >> 29) & 3));
        const PyInt page = static_cast<PyInt>(i * 4 & ~std::size_t(0xfff)) + (imm << 12);
        if (!(lo - 0x1000 <= page && page < hi)) continue;
        for (std::size_t j = i + 1; j < std::min(i + 9, words); ++j) {
            const std::uint32_t y = w(j);
            if ((y & 0xffc00000) == 0x91000000 && ((y >> 5) & 31) == rd) {
                const PyInt t = page + ((y >> 10) & 0xfff);
                if (lo <= t && t < hi) out[t].push_back(static_cast<PyInt>(j) * 4);
                break;
            }
        }
    }
    return out;
}

}  // namespace

Found RawShaders(Context& cx, Bytes d, const std::string& label, const std::set<std::string, std::less<>>& known) {
    std::vector<std::pair<PyInt, PyInt>> skip;
    FindAll(d, kGlslcMagic, [&](std::int64_t m) {
        const GlslcLayout l = GlslcLayoutAt(d, m);
        if ((l.first == 0xa8 ? 0 : 0x760) < l.size && l.size <= Len(d) - m) skip.emplace_back(m, m + l.size);
    });
    FindAll(d, Lit("BNSH\0\0\0\0"), [&](std::int64_t m) { skip.emplace_back(m, m + U32(d, m + 0x1c)); });
    std::vector<std::int64_t> ctls;
    FindAll(d, kCtlMagic, [&](std::int64_t c) {
        if (c & 3) return;
        for (const auto& [a, b] : skip) {
            if (a <= c && c < b) return;
        }
        if (known.count(Slice(d, c, c + U32(d, c + 0x20)))) return;
        ctls.push_back(c);
    });
    if (ctls.empty()) return {};
    const RelTargets rel = RelTargetsOf(d);
    const std::set<std::int64_t> cset(ctls.begin(), ctls.end());
    std::map<PyInt, PyInt> code_of;
    for (const auto& [s, c] : rel.items) {
        if (c < 0 || c > std::numeric_limits<std::int64_t>::max() || !cset.count(static_cast<std::int64_t>(c)))
            continue;
        const PyInt* before = rel.Get(static_cast<PyInt>(s) - 8);
        if (!before || code_of.count(c)) continue;
        if (U32(d, static_cast<PyInt>(s) + 8) == U32(d, c + 0x704)) code_of[c] = *before;
    }
    std::map<PyInt, std::vector<PyInt>> built;
    if (std::any_of(ctls.begin(), ctls.end(), [&](std::int64_t c) { return !code_of.count(c); })) {
        built = AdrpTargets(d, *std::min_element(ctls.begin(), ctls.end()) - 0x100000,
                            *std::max_element(ctls.begin(), ctls.end()) + 0x100000);
    }
    const auto near = [&](PyInt k, PyInt c) {
        PyInt best = -1;
        for (const PyInt a : built.at(k)) {
            for (const PyInt b : built.at(c)) {
                const PyInt gap = a > b ? a - b : b - a;
                if (best < 0 || gap < best) best = gap;
            }
        }
        return best;
    };
    Found out;
    for (const std::int64_t c : ctls) {
        const PyInt nctl = U32(d, c + 0x20), n = U32(d, c + 0x704);
        const Bytes ctl = Slice(d, c, c + nctl);
        bool taken = false;
        std::vector<PyInt> cands;
        if (code_of.count(c)) {
            cands.push_back(code_of[c]);
        } else {
            for (const PyInt k : {(c + nctl + 15) & ~PyInt(15), c - n}) {
                if (built.count(k) && built.count(c) && near(k, c) < 0x400) cands.push_back(k);
            }
            std::stable_sort(cands.begin(), cands.end(), [&](PyInt a, PyInt b) { return near(a, c) < near(b, c); });
        }
        for (const PyInt k : cands) {
            const Bytes code = Slice(d, k, k + n);
            const std::string st = k >= 0 && !Contains(code, kCtlMagic) ? GlslcStage(ctl, code) : "";
            if (!st.empty() && !Check(st, ctl, code)) {
                out.push_back(Make(absl::StrCat(label, "#RAW@", Hex(c)), st, ctl, code));
                taken = true;
                break;
            }
        }
        if (!taken) {
            cx.print(
                absl::StrCat("[RED] ", label, " control section @", Hex(c),
                             ": no code area: no data record points at one, and the code builds the address of none "
                             "right after or right before it; left out"));
        }
    }
    return out;
}

namespace {

// retro_lzss8: Retro's LZSS on 8-bit units -> the first n bytes, or nullopt when the stream runs out or points before
// its start
std::optional<std::string> RetroLzss8(Bytes s, PyInt n) {
    if (n > 10 * (Len(s) + 1)) return std::nullopt;  // more than the stream can hold: it runs out first
    std::string out;
    std::size_t i = 0;
    unsigned flags = 0;
    int left = 0;
    while (static_cast<PyInt>(out.size()) < n) {
        if (!left) {
            if (i >= s.size()) return std::nullopt;
            flags = static_cast<std::uint8_t>(s[i++]);
            left = 8;
        }
        if (flags & 0x80) {
            if (i + 1 >= s.size()) return std::nullopt;
            const std::size_t ln = (static_cast<std::uint8_t>(s[i]) >> 4) + 3;
            const std::size_t dist = (static_cast<std::size_t>(static_cast<std::uint8_t>(s[i]) & 0xf) << 8) |
                                     static_cast<std::uint8_t>(s[i + 1]);
            i += 2;
            if (!(0 < dist && dist <= out.size())) return std::nullopt;
            for (std::size_t k = 0; k < ln; ++k) out.push_back(out[out.size() - dist]);
        } else {
            if (i >= s.size()) return std::nullopt;
            out.push_back(s[i++]);
        }
        flags = (flags << 1) & 0xff;
        --left;
    }
    out.resize(static_cast<std::size_t>(n));
    return out;
}

}  // namespace

void RetroMainForms(Bytes d, const std::string& label) {
    FindAll(d, "RFRM", [&](std::int64_t p) {
        if (p < 5 || static_cast<std::uint8_t>(d[static_cast<std::size_t>(p - 5)]) != 1 ||
            Slice(d, p - 4, p - 1) != Lit("\0\0\0"))
            return;
        const std::optional<std::string> head = RetroLzss8(From(d, p - 1), 0x20);
        if (!head || Head(*head, 4) != "RFRM" || Slice(*head, 0x14, 0x18) != "MTRL") return;
        const std::uint64_t length = U64(*head, 4) < U64Be(*head, 4) ? U64(*head, 4) : U64Be(*head, 4);
        if (RetroLzss8(From(d, p - 1), 0x20 + static_cast<PyInt>(length))) {
            throw NotPorted(absl::StrCat(
                label, "@", Hex(p - 5),
                ": a Retro Studios material archive (RFRM MTRL): its SNVN reader is not ported to nxmac yet"));
        }
    });
}

void SlctShaders(Bytes d, PyInt base, const std::string& label) {
    if (base + 72 > Len(d)) return;
    const PyInt c = U32(d, base + 40), cn = U32(d, base + 44), k = U32(d, base + 48), kn = U32(d, base + 52);
    const PyInt size = std::max(c + cn, k + kn);
    if (std::min(c, k) < 72 || size > Len(d) - base) return;
    throw NotPorted(absl::StrCat(label, "#SLCT@", Hex(base),
                                 ": a Monolith SLCT shader: its HCPS reader is not ported to nxmac yet"));
}

bool HeapsPak(Bytes file) {
    const Bytes head = Head(file, 12);
    if (Len(head) < 12 || Head(head, 3) != "PAK" || static_cast<std::uint8_t>(head[3]) > 1) return false;
    const std::int32_t size = I32(head, 4), dsize = I32(head, 8);
    if (!(12 < size && size <= (1 << 28)) || dsize < 0) return false;
    const Bytes h = Slice(file, 12, size);
    int depth = 0;
    std::function<PyInt(PyInt)> entry = [&](PyInt p) -> PyInt {
        if (++depth > 980) throw PyError("RecursionError", "maximum recursion depth exceeded");
        const PyInt n = Index(h, p);
        const Bytes name = Slice(h, p + 1, p + 1 + n);
        if (!ValidUtf8(name)) throw PyError("UnicodeDecodeError", "invalid utf-8");
        const std::uint8_t fl = Index(h, p + 1 + n);
        p += n + 2;
        if (fl & 1) {
            const std::int32_t cnt = I32(h, p);
            p += 4;
            for (std::int32_t k = 0; k < cnt; ++k) p = entry(p);
            --depth;
            return p;
        }
        double pos = 0;
        if (fl & 2) {
            pos = F64(h, p);
            p += 8;
        } else {
            I32(h, p);
            p += 4;
        }
        NeedStruct(h, p, 8);
        // int(pos) of a NaN or an infinity: an error heaps_pak_files does not catch
        if (std::isnan(pos)) throw PyError("ValueError", "cannot convert float NaN to integer");
        if (std::isinf(pos)) throw PyError("OverflowError", "cannot convert float infinity to integer");
        --depth;
        return p + 8;
    };
    PyInt end = 0;
    try {
        end = entry(static_cast<std::uint8_t>(head[3]) ? 64 : 0);
    } catch (const PyError& e) {
        if (e.type() == "ValueError" || e.type() == "OverflowError") throw;
        return false;
    }
    return Slice(h, end, end + 4) == "DATA";
}

bool UnitySerialized(Bytes d) {
    if (Len(d) < 48) return false;
    const std::uint32_t ver = U32Be(d, 8);
    if (!(17 <= ver && ver <= 22) || static_cast<std::uint8_t>(d[16]) > 1) return false;
    PyInt msz = 0, fsz = 0, doff = 0;
    if (ver >= 22) {
        msz = U32Be(d, 20);
        fsz = static_cast<std::int64_t>(U64Be(d, 24));
        doff = static_cast<std::int64_t>(U64Be(d, 32));
    } else {
        msz = U32Be(d, 0);
        fsz = U32Be(d, 4);
        doff = U32Be(d, 12);
    }
    return fsz == Len(d) && 0 < msz && msz < fsz && doff <= fsz;
}

bool Ue3Package(Bytes head) {
    return Head(head, 4) == Lit("\xc1\x83\x2a\x9e") && Len(head) >= 6 && U16(head, 4) > 0 && U16(head, 4) < 0x8000;
}

std::vector<std::pair<std::string, bool>> DynSyms(Bytes img) {
    const PyInt mod0 = U32(img, 4);
    if (Slice(img, mod0, mod0 + 4) != "MOD0") throw PyError("AssertionError", "no MOD0");
    const PyInt dyn = mod0 + I32(img, mod0 + 4);
    std::map<std::int64_t, std::uint64_t> tags;
    for (int i = 0; i < 4096; ++i) {
        NeedStruct(img, dyn + 16 * i, 16);
        const std::int64_t t = I64(img, dyn + 16 * i);
        if (t == 0) break;
        tags.emplace(t, U64(img, dyn + 16 * i + 8));
    }
    if (!tags.count(6)) throw PyError("KeyError", "6");
    if (!tags.count(5)) throw PyError("KeyError", "5");
    const PyInt symtab = tags[6], strtab = tags[5];
    PyInt n = 0;
    if (tags.count(4)) {
        n = U32(img, static_cast<PyInt>(tags[4]) + 4);
    } else {
        const PyInt gap = strtab - symtab;  // Python's floor division
        n = gap >= 0 ? gap / 24 : -((-gap + 23) / 24);
    }
    std::vector<std::pair<std::string, bool>> out;
    for (PyInt i = 1; i < n; ++i) {
        const PyInt at = symtab + i * 24;
        NeedStruct(img, at, 24);
        const PyInt name = U32(img, at);
        const std::uint16_t shndx = U16(img, at + 6);
        const std::int64_t end = Find(img, Lit("\0"), strtab + name);
        if (end < 0) throw PyError("ValueError", "subsection not found");
        const Bytes s = Slice(img, strtab + name, end);  // latin-1: the bytes as they are
        if (!s.empty()) out.emplace_back(std::string(s), shndx != 0);
    }
    return out;
}

int GlslSources(Bytes img) {
    const auto word = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    // \bvoid\s+main\s*\(
    const auto has_main = [&](Bytes s) {
        for (std::size_t at = s.find("void"); at != Bytes::npos; at = s.find("void", at + 1)) {
            if (at && word(s[at - 1])) continue;
            std::size_t i = at + 4;
            if (i >= s.size() || !space(s[i])) continue;
            while (i < s.size() && space(s[i])) ++i;
            if (s.substr(i, 4) != "main") continue;
            i += 4;
            while (i < s.size() && space(s[i])) ++i;
            if (i < s.size() && s[i] == '(') return true;
        }
        return false;
    };
    int n = 0;
    for (std::size_t at = img.find("#version "); at != Bytes::npos;) {
        std::size_t end = img.find('\0', at);
        if (end == Bytes::npos) end = img.size();
        if (has_main(img.substr(at, end - at))) ++n;
        at = img.find("#version ", end);
    }
    return n;
}

std::set<VarPair> VarPairs(const Context& cx, const Found& found) {
    std::map<std::string, std::map<std::string, std::string>> var;
    for (const Shader& s : found) {
        std::string g;
        if (absl::StrContains(s.where, "#var")) {
            g = s.where;
        } else if (const auto it = cx.bahs_var.find(s.where); it != cx.bahs_var.end()) {
            g = it->second;
        }
        if (!g.empty()) var[g][s.st] = Skey(s.ctl, s.code, cx.skip);
    }
    std::set<VarPair> out;
    for (const auto& [g, v] : var) {
        const auto ps = v.find("ps");
        if (ps == v.end()) continue;
        for (const char* st : {"vs", "ms"}) {
            const auto it = v.find(st);
            if (it != v.end()) out.insert({it->second, ps->second, absl::StrContains(g, "#BAHS#prog")});
        }
    }
    return out;
}

}  // namespace nxmac::shader::internal
