#include "scan.h"

#include <unordered_map>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "archive.h"

namespace nxmac::shader::internal {
namespace {

constexpr Bytes kSumoMux = Lit("MuxFDMR\x02");
constexpr Bytes kSumoNx = Lit("Ninty_NX\0\0\0\0\0\0\0\0");
constexpr Bytes kNvnsh = Lit("NVNsh\0");
constexpr Bytes kZstd = Lit("\x28\xb5\x2f\xfd");
constexpr Bytes kLz4Frame = Lit("\x04\x22\x4d\x18");

void Append(Found& out, Found more) {
    out.insert(out.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
}

[[noreturn]] void Unported(const std::string& label, const std::string& what) {
    throw NotPorted(absl::StrCat(label, ": ", what, ": its reader is not ported to nxmac yet"));
}

bool PgStage(Bytes head) {
    for (const Bytes m : {Lit("VSO\0"), Lit("HSO\0"), Lit("DSO\0"), Lit("GSO\0"), Lit("PSO\0"), Lit("CSO\0")}) {
        if (head == m) return true;
    }
    return false;
}

}  // namespace

void ScanBlob(Context& cx, Bytes d, const std::string& label, Found& out, int depth, Budget* left) {
    if (depth > kNest) Fail(absl::StrCat(label, ": archives nested more than ", kNest, " deep"));
    Budget own;
    if (Head(d, 8) == kSumoMux) {
        const std::vector<std::pair<std::string, std::string>> members = SumoMembers(d, label);
        if (!left) {  // as with Yaz0, count decoded bytes, not compressed bytes
            PyInt total = 0;
            for (const auto& m : members) total += Len(m.second);
            own = {kNest * total, kNest * total};
            left = &own;
        }
        for (const auto& [nm, member] : members) ScanBlob(cx, member, label + nm, out, depth + 1, left);
        return;
    }
    if (Head(d, 4) == "HCPS" || Head(d, 4) == "xbc1" || Head(d, 4) == "DRSM" || Head(d, 8) == Lit("cmp\0zlib")) {
        Unported(label, "a Monolith shader container");
    }
    FindAll(d, Lit("SLCT\x03\x10\0\0"), [&](std::int64_t at) { SlctShaders(d, at, label); });
    const PyInt stored = Len(d);  // what the member takes up in its archive
    std::string decoded;
    if (Head(d, 4) == "Yaz0") {
        if (left) {  // charged by its header before it is decoded
            PyInt n = 0;
            for (const char c : Slice(d, 4, 8)) n = n << 8 | static_cast<std::uint8_t>(c);
            left->expanded -= n;
            if (left->expanded < 0)
                Fail(absl::StrCat(label, ": the Yaz0 archive members expand to more than ", kNest, " times the file"));
        }
        decoded = Yaz0(d, label);
        d = decoded;
    }
    if (!left) {  // the file itself: what its archives' members may add up to, and what their Yaz0 may expand to
        own = {kNest * Len(d), kNest * Len(d)};
        left = &own;
    } else {
        left->members -= stored;
        if (left->members < 0) {
            Fail(absl::StrCat(label, ": the archive members add up to more than ", kNest,
                              " times the file (members that overlap)"));
        }
    }
    const Bytes magic = Head(d, 4);
    if (magic == "SARC") {
        SarcMembers(d, label, [&](const std::string& nm, Bytes m) {
            ScanBlob(cx, m, absl::StrCat(label, "/", nm), out, depth + 1, left);
        });
        return;
    }
    if (magic == "NARC") Unported(label, "a Camelot NARC container");
    if (magic == "BAHS") return Append(out, BahsShaders(cx, d, label));
    if (magic == "rgxd") return Append(out, RgxdShaders(cx, d, label));
    if (magic == "NDSR") return Append(out, NdsrShaders(cx, d, label));
    if (magic == "NDVE") return Append(out, NdveShaders(cx, d, label));
    if (Found s = SeadPrimShaders(d, label); !s.empty()) return Append(out, std::move(s));
    if (Head(d, 11) == Lit(".tfxo file\0")) return Append(out, TfxoShaders(d, label));
    if (magic == "MSHD") return Append(out, MshdShaders(d, label));
    if (magic == Lit("DAT\0") && Len(d) >= 0x1c) {
        if (const auto members = PgdatMembers(d)) {  // PlatinumGames
            for (const auto& [nm, m] : *members) ScanBlob(cx, m, absl::StrCat(label, "/", nm), out, depth + 1, left);
            return;
        }
    }
    if (PgStage(magic) && Len(d) >= 0x18) {
        const PyInt o = U32(d, 0xc);
        if (Slice(d, o, o + 4) == kCtlMagic) return Append(out, PgShader(d, label));
    }
    if (Found s = PgSizedShader(d, label); !s.empty()) return Append(out, std::move(s));
    if (std::optional<Found> s = ViperShader(d, label))
        return Append(out, std::move(*s));  // Star Fox's shader binaries
    if (magic == "VIPR") {                  // Star Fox's resource containers: NDSR upscaler networks inside
        FindAll(d, "NDSR", [&](std::int64_t m) {
            if (m + 0x20 <= Len(d) && Slice(d, m + 4, m + 6) == Lit("\0\x01")) {
                Append(out, NdsrShaders(cx, From(d, m), absl::StrCat(label, "#NDSR@", Hex(m))));
            }
        });
        return;
    }
    FindAll(d, Lit("BNSH\0\0\0\0"), [&](std::int64_t m) {
        if (m + 0x20 > Len(d)) return;  // too near the end for a BNSH header
        const PyInt size = U32(d, m + 0x1c);
        if (m + size <= Len(d) && Slice(d, m + 0x60, m + 0x64) == "grsc") Append(out, BnshShaders(cx, d, m, label));
    });
    if (Contains(d, "MGFX")) Append(out, MgfxShaders(d, label));
    if (Contains(d, "HNSHV002")) Append(out, NeedleShaders(d, label));
    if (Contains(d, kSumoNx) || Contains(d, kNvnsh)) Append(out, SumoShaders(d, label));
}

Keyed UniqKeys(const Found& found, const KeySkip& skip) {
    Keyed uniq;
    std::unordered_map<std::string, std::size_t> at;
    for (const Shader& s : found) {
        std::string key = Skey(s.ctl, s.code, skip);
        const auto it = at.find(key);
        if (it != at.end()) {
            const Shader& first = uniq[it->second].second;
            if (!SameShader(first.ctl, first.code, s.ctl, s.code, skip)) {
                Fail(absl::StrCat("key ", key, " is the same but the control section or code area differs (", s.where,
                                  ")"));
            }
            continue;
        }
        at.emplace(key, uniq.size());
        uniq.emplace_back(std::move(key), s);
    }
    return uniq;
}

namespace {

// shader_split.py's scan_file_in
void ScanFileIn(Context& cx, const RomFsSource& src, const RomFsSource::File& file, const std::string& rel,
                Found& found) {
    const auto read = [&](std::uint64_t n) {
        absl::StatusOr<std::string> bytes = src.Read(file, n);
        if (!bytes.ok()) throw PyError("OSError", std::string(bytes.status().message()));
        return *std::move(bytes);
    };
    const std::string& path = file.rel;  // the source's own name for it: its neighbours are looked up beside it
    const std::size_t slash = path.rfind('/');
    const std::string base = path.substr(slash == std::string::npos ? 0 : slash + 1);
    // os.path.splitext: the last dot of the name, not one that only leads it
    const std::size_t dot = base.rfind('.');
    const bool has_ext = dot != std::string::npos && base.find_first_not_of('.') < dot;
    const std::string ext = has_ext ? base.substr(dot) : "";
    const std::string stem = path.substr(0, path.size() - ext.size());
    if ((ext == ".data" || ext == ".patch" || ext == ".debug" || ext == ".nxpc") && src.Exists(stem + ".dict")) {
        return;  // read through its Next Level Games index
    }
    const std::string label = absl::StrCat("/", rel);
    std::string head = read(16);
    if (Head(head, 4) == Lit("\x58\x24\xf3\xa9")) Unported(label, "a Next Level Games archive index");
    if (absl::EndsWith(path, ".pkdswitch") && src.Exists(path.substr(0, path.size() - 10) + ".pkiswitch")) {
        Unported(label, "a Double Eleven resource pack");
    }
    if (Head(head, 8) == "IDRK0000" || Head(head, 8) == "PDRK0000") Unported(label, "Koei Tecmo's RDB data");
    if (Head(head, 4) == "bnd2") Unported(label, "a Criterion Bundle 2");
    std::string d;
    if (Head(head, 3) ==
        "PAK") {  // a Heaps PAK archive (Dead Cells), or another engine's .pak that shares its first bytes
        d = read(0);
        if (HeapsPak(d)) Unported(label, "a Heaps PAK archive");
    }
    if (Head(head, 4) == "SPKG" || (Head(head, 4) == Lit("\0\0\1\0") && absl::EndsWith(path, ".gz"))) {
        Unported(label, "Koei Tecmo's shader packages");
    }
    if (Head(head, 4) == "BMSG" || Ue3Package(head)) Unported(label, "an Unreal Engine 3 shader cache or package");
    if (Head(head, 4) == "RFRM") Unported(label, "a Retro Studios form");
    if (Head(head, 4) == "BERG") Unported(label, "Snowdrop's data slices");
    if (Head(head, 4) == Lit("ARC\0")) Unported(label, "an MT Framework archive");
    if (Head(head, 4) == "SCNE") Unported(label, "a Bezel Engine archive (zstd entries)");
    if (Head(head, 8) == "GFLXPACK") Unported(label, "a Game Freak GFPAK archive");
    if (Head(head, 4) == "SRYK") Unported(label, "a VBF archive");
    if (Head(head, 4) == kZstd) Unported(label, "a zstd file");
    if (Head(head, 3) != "PAK") d = read(0);
    std::string held;                         // what d decoded into, when it is not the file's bytes
    if (Head(d, 3) == Lit("\x1f\x8b\x08")) {  // standard gzip
        held = Ungzip(d, label);
        d = held;
        head = Head(d, 16);
    }
    if (Slice(head, 4, 5) == "\x78" && absl::EndsWith(path, ".gz")) {  // Gust's chunked zlib
        if (std::optional<std::string> z = GustGz(d)) {
            held = *std::move(z);
            d = held;
        }
    }
    if (Head(d, 4) == "HCPS" || Head(d, 4) == "xbc1" || Head(d, 4) == "DRSM" || Head(d, 8) == Lit("cmp\0zlib")) {
        Unported(label, "a Monolith shader container");
    }
    if (Head(d, 4) == "RYHP" || Slice(d, 8, 12) == kGfxMagic) {
        Append(found, PhyreShaders(d, label));
        return;
    }
    if (Head(head, 4) != kZstd && (Slice(head, 4, 8) == kZstd || Slice(head, 4, 8) == kLz4Frame)) {
        // HAL's .cmp containers: a size, then a zstd or LZ4 frame; bytes whose frame records another size are not one
        const std::optional<std::uint64_t> size = FrameSize(From(d, 4));
        if (!size || *size == U32(d, 0)) Unported(label, "a size-prefixed zstd or LZ4 frame");
    }
    std::string blob;
    Bytes view = d;
    if (Head(view, 4) == "Yaz0") {
        blob = Yaz0(view, label);
        view = blob;
    }
    if (Head(view, 3) == "XNB" && Index(view, 4) == 5 && (Index(view, 5) & 0xc0) && Len(view) > 14) {
        Unported(label, "a compressed XNB file");
    }
    if (Head(view, 8) == Lit("UnityFS\0") || (Slice(view, 8, 11) == Lit("\0\0\0") && UnitySerialized(view))) {
        Unported(label, "a Unity file");
    }
    const Bytes m4 = Head(view, 4);
    const bool candidate = Contains(view, Lit("BNSH\0\0\0\0")) || Contains(view, "BAHS") || m4 == "MSHD" ||
                           m4 == "NDSR" || m4 == "NARC" || m4 == "NDVE" || (m4 == "SARC" && Contains(view, "Yaz0")) ||
                           Contains(view, kGlslcMagic) || Contains(view, "MGFX") || Contains(view, "HNSHV002") ||
                           Head(view, 8) == kSumoMux || Contains(view, kSumoNx) || Contains(view, kNvnsh);
    if (!candidate) return;
    ScanBlob(cx, view, label, found);
    // whole GLSLC outputs in a file no other reader takes
    if (found.empty() && m4 != "MSHD" && Contains(view, kGlslcMagic)) Append(found, GlslcShaders(view, label, false));
}

}  // namespace

FileResult ScanFile(const RomFsSource& src, const RomFsSource::File& file, const std::string& rel, const KeySkip& skip,
                    const std::function<void(const std::string&)>& print) {
    FileResult r;
    r.rel = rel;
    Context cx{skip, {}, print};
    std::string why;
    try {
        Found found;
        ScanFileIn(cx, src, file, rel, found);
        r.uniq = UniqKeys(found, skip);
        r.pairs = VarPairs(cx, found);
        return r;
    } catch (const Red& e) {
        why = e.what();
    } catch (const PyError& e) {
        why = absl::StrCat(e.type(), ": ", e.what());
    }
    // most red lines name the file already (unreadable_why)
    const auto lstrip = [](std::string_view text) {
        while (!text.empty() && text.front() == '/') text.remove_prefix(1);
        return text;
    };
    if (!absl::StartsWith(lstrip(why), lstrip(rel))) why = absl::StrCat(rel, ": ", why);
    print(absl::StrCat("[RED] ", why, " (its shaders are left out)"));
    r.uniq.clear();
    r.pairs.clear();
    r.unreadable = why;
    return r;
}

std::optional<std::string> UnportedArchive(const RomFsSource& src, const RomFsSource::File& file) {
    const std::string& p = file.rel;
    const std::size_t slash = p.rfind('/');
    const std::string dir = slash == std::string::npos ? "" : p.substr(0, slash + 1);
    const std::string name = p.substr(dir.size());
    const auto beside = [&](const std::string& n) { return src.Exists(dir + n); };
    const auto has = [&](std::string_view suffix) { return absl::EndsWith(p, suffix); };
    // LINKDATA archives (Koei Tecmo): an index beside the data
    if ((name == "DATA1.bin" && beside("DATA0.bin")) || (name == "LINKDATA.BIN" && beside("LINKDATA.IDX")) ||
        (absl::StartsWith(name, "LD_") && absl::EndsWith(name, ".BIN") && name.size() >= 7 &&
         beside(absl::StrCat("LI_", name.substr(3, name.size() - 7), ".IDX")))) {
        return "a LINKDATA archive";
    }
    if (has(".bdt") && src.Exists(p.substr(0, p.size() - 4) + ".bhd5")) return "a FromSoftware dvdbnd archive";
    if (has(".trpfs") && src.Exists(p.substr(0, p.size() - 1) + "d")) {
        absl::StatusOr<std::string> head = src.Read(file, 8);
        if (head.ok() && *head == Lit("ONEPACK\0")) return "a Trinity pack file (Oodle)";
    }
    if (p == "Data/layout.toc" || absl::EndsWith(p, "/Data/layout.toc")) return "a Frostbite layout (Oodle)";
    absl::StatusOr<std::string> head = src.Read(file, 16);
    if (!head.ok()) return std::nullopt;  // the scan reports it
    const Bytes h = *head;
    if (has(".pak") && Head(h, 4) == "KPKA") return "an RE Engine PAK archive";
    if (has(".pak")) {
        // Unreal's pak: its magic 0x5A6F12E1 in one of its footers (uepak.magic_back)
        const std::uint64_t size = src.Size(file);
        for (const auto& [back, foot] : {std::pair<int, int>{0x2c, 0x2d}, {0xc8, 0xc8 + 21}, {0xcc, 0xcc + 21}}) {
            if (size < static_cast<std::uint64_t>(foot)) continue;
            absl::StatusOr<std::string> magic = src.ReadAt(file, size - back, 4);
            if (magic.ok() && Len(*magic) == 4 && U32(*magic, 0) == 0x5A6F12E1) return "an Unreal pak";
        }
    }
    if (has(".arc") && Head(h, 8) == Lit("\x10\x32\x54\x76\x98\xef\xcd\xab"))
        return "Super Smash Bros. Ultimate's data.arc";
    if (has(".pkz") && Head(h, 4) == "pkzl") return "a PlatinumGames PKZ archive (Oodle)";
    if (absl::EndsWith(absl::AsciiStrToLower(p), ".cpk") && Head(h, 4) == "CPK ") return "a CRI CPK archive";
    if (Head(h, 4) == "CRA.") return "a Camelot CRA archive";
    if (Head(h, 4) == Lit("PK\x03\x04") || Head(h, 4) == "VZIP") return "a zip archive";
    if (has(".arh") && Head(h, 4) == "arh1") return "a Monolith ARH1 archive";
    if (Head(h, 4) == "PDAT") return "a phx PDAT archive";
    if (Head(h, 4) == "RPF6" && Len(h) >= 16) return "a Rockstar RPF6 archive";
    return std::nullopt;
}

}  // namespace nxmac::shader::internal
