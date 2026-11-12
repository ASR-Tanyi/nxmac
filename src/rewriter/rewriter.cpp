// The rewrite from start to end (patch_nso.py's top level): the layout of the image and its patch area, the build's
// patches, the scan for rewrite sites, the stubs in the order the script emits them, and the files.
#include <algorithm>
#include <cstring>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "match.h"
#include "rewriter_impl.h"
#include "text.h"

namespace nxmac::rewriter {
namespace internal {
namespace {

using namespace a64;  // NOLINT: the encoders

// Observation gate numbers: OBS_* of the host's dm.h
constexpr std::uint32_t kObsRegionAsk = 22;
constexpr std::uint32_t kObsRegionGot = 23;
constexpr std::uint32_t kObsRegionFree = 24;
constexpr std::uint32_t kObsStdallocInit = 25;

constexpr std::string_view kAllocateAddressRegion = "_ZN2nn2os6detail11VammManager21AllocateAddressRegionEPmm";
constexpr std::string_view kFreeAddressRegion = "_ZN2nn2os6detail11VammManager17FreeAddressRegionEm";
constexpr std::string_view kStdallocInit = "_ZN2nn3mem17StandardAllocator10InitializeEPvm";
constexpr std::string_view kStdallocInitBool = "_ZN2nn3mem17StandardAllocator10InitializeEPvmb";

Addr Align(Addr value, Addr alignment) { return (value + alignment - 1) & ~(alignment - 1); }

bool Contains(Bytes image, Addr from, Addr to, std::string_view needle) {
    if (from < 0 || to > static_cast<Addr>(image.size()) || to - from < static_cast<Addr>(needle.size())) return false;
    const auto* begin = image.data() + from;
    const auto* end = image.data() + to;
    return std::search(begin, end, needle.begin(), needle.end(),
                       [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); }) != end;
}

std::string Utf16(std::string_view ascii) {
    std::string out;
    for (const char c : ascii) {
        out += c;
        out += '\0';
    }
    return out;
}

}  // namespace

std::string_view KindName(Kind kind) {
    switch (kind) {
        case Kind::kSvc:
            return "svc";
        case Kind::kTpidrR:
            return "tpidr_r";
        case Kind::kTpidrW:
            return "tpidr_w";
        case Kind::kTpidrroR:
            return "tpidrro_r";
        case Kind::kCntpct:
            return "cntpct";
        case Kind::kCntvct:
            return "cntvct";
        case Kind::kCtr:
            return "ctr";
        case Kind::kSpAlign:
            return "sp_align";
        case Kind::kX18:
            return "x18";
    }
    return "";
}

// The stub context of a build's frame step or stubs (patch_nso.py stub_site, stub_put, stub_address)
class StubContextImpl final : public StubContext {
public:
    StubContextImpl(Rewriter* rewriter, std::string what, std::string stat)
        : r_(*rewriter), what_(std::move(what)), stat_(std::move(stat)) {}

    Addr Here() const override { return r_.Here(); }
    void Emit(std::uint32_t word) override { r_.Emit(word); }
    using StubContext::Emit;

    void Site(Addr off, std::string_view original) override {
        const std::optional<std::vector<std::uint8_t>> ob = HexBytes(original);
        if (!ob) Fail(absl::StrFormat("%s site %s: its original bytes are not hex digits", what_, Hex(off)));
        const auto length = static_cast<Addr>(ob->size());
        if (off < 0 || off + length > static_cast<Addr>(r_.img_.size()) ||
            std::memcmp(r_.img_.data() + off, ob->data(), ob->size()) != 0) {
            const Addr have = std::clamp<Addr>(static_cast<Addr>(r_.img_.size()) - off, 0, length);
            Fail(absl::StrFormat("%s original bytes mismatch %s: %s", what_, Hex(off),
                                 off >= 0 ? BytesHex(r_.img_.data() + off, static_cast<std::size_t>(have)) : ""));
        }
        for (Addr o = off; o < off + length; o += 4) {
            if (r_.kinds_.contains(o) || r_.InWindow(o)) {
                Fail(absl::StrFormat("%s site is a rewrite site %s", what_, Hex(o)));
            }
        }
    }

    void Put(Addr off, const std::vector<std::uint32_t>& words, const std::string& why) override {
        const std::size_t length = 4 * words.size();
        if (off < 0 || static_cast<std::uint64_t>(off) + length > r_.img_.size()) FailRange(off, length);
        const std::string old = BytesHex(r_.img_.data() + off, length);
        for (std::size_t i = 0; i < words.size(); ++i) r_.PutWord(off + 4 * static_cast<Addr>(i), words[i]);
        r_.cuts_.push_back({off, old, BytesHex(r_.img_.data() + off, length), why});
        r_.Stat(stat_);
    }

    Addr Address(Addr off) const override {
        const std::uint32_t a = U32(r_.orig_text_, off);
        const std::uint32_t b = U32(r_.orig_text_, off + 4);
        if ((a & 0x9f000000) != 0x90000000) Fail(absl::StrFormat("%s: no adrp at %s", what_, Hex(off)));
        const Addr imm = static_cast<Addr>(((a >> 5) & 0x7ffff) << 2 | ((a >> 29) & 3)) << 12;
        const Addr page = (off & ~Addr{0xfff}) + ((imm & (Addr{1} << 32)) != 0 ? imm - (Addr{1} << 33) : imm);
        if (((b >> 5) & 31) != (a & 31)) {
            Fail(
                absl::StrFormat("%s: the instruction after the adrp at %s does not use its register", what_, Hex(off)));
        }
        if ((b & 0xffc00000) == 0x91000000) return page + ((b >> 10) & 0xfff);  // add xD, xN, #imm
        if ((b & 0x3b000000) != 0x39000000) {
            Fail(absl::StrFormat("%s: no add, load or store after the adrp at %s", what_, Hex(off)));
        }
        // An unsigned-offset load or store, scaled by the access size (q: 16)
        const Addr scale = (Addr{1} << (b >> 30)) * ((b & 0x04800000) == 0x04800000 && (b >> 30) == 0 ? 16 : 1);
        return page + ((b >> 10) & 0xfff) * scale;
    }

private:
    Rewriter& r_;
    std::string what_;
    std::string stat_;
};

Rewriter::Rewriter(const Module& module, const RewriteOptions& options, Addr patch_front)
    : module_(module),
      options_(options),
      tables_(options.tables != nullptr ? *options.tables : PatchTables::BuiltIn()),
      name_(options.name),
      build_id_(module.BuildId()) {
    Layout(patch_front);
}

void Rewriter::Stat(std::string_view key, int count) {
    for (auto& [name, n] : stats_) {
        if (name == key) {
            n += count;
            return;
        }
    }
    stats_.emplace_back(key, count);
}

Insn Rewriter::Dis(std::uint32_t w, Addr addr) const {
    std::optional<Insn> ins = md_.One(w, addr);
    if (!ins) Fail(absl::StrFormat("the word %s at %s is no instruction", Hex8(w), Hex(addr)));
    return *std::move(ins);
}

bool Rewriter::InWindow(Addr off) const {
    return std::any_of(window_ranges_.begin(), window_ranges_.end(),
                       [&](const Range& w) { return w.first <= off && off < w.second; });
}

void Rewriter::Layout(Addr patch_front) {
    // Every jump between the patch area and the code is a B (128 MB either way). A patch area appended after the image
    // (with bss) cannot reach the code when it would start beyond 112 MB (16 MB are left for the patch area itself):
    // the patch area then sits FRONT bytes before the module base (PATCH_OFF is negative), and the output file holds
    // only text + ro + data followed by the patch area; the loader supplies zero pages for bss and the key page.
    // A text that ends within 32 MB of the reach puts the patch area right before the base (BIG_TEXT), tried first with
    // the gap that keeps every site in reach and then with exactly the size it needs. A text longer than the reach
    // itself splits the patch area in two (TWO): the front area takes the sites below SPLIT, the back area, right
    // after the image, the others.
    const auto image_len = static_cast<Addr>(module_.image.size());
    bss_ = static_cast<Addr>(module_.bss);
    text_end_ = static_cast<Addr>(module_.text_end());
    const Addr near_patch_off = Align(Align(image_len + bss_, 0x4000) + 0x4000, 0x10000);
    far_ = near_patch_off > 0x7000000;
    front_ = 0x2000000;
    big_text_ = far_ && text_end_ + front_ > 0x8000000 - 0x10000;
    two_ = big_text_ && text_end_ > 0x8000000 - 0x10000;
    patch_front_given_ = patch_front != 0;
    if (two_) {
        back_min_ = Align(Align(image_len + bss_, 0x4000) + 0x8000, 0x10000) - 0x8000000 + 0x100000;
        if (back_min_ >= 0x8000000 - 0x200000) {
            Fail(
                absl::StrFormat("no split reaches: the back patch area reaches no site below %s, too close to the "
                                "front area's reach",
                                Hex(back_min_)));
        }
        front_ = patch_front != 0 ? patch_front
                                  : std::min<Addr>(0x1000000, (0x8000000 - 0x100000 - back_min_) & ~Addr{0xFFFF});
        split_ = std::max<Addr>(0x8000000 - front_ - 0x100000, back_min_);
    } else if (big_text_) {
        front_ = patch_front != 0 ? patch_front : (0x8000000 - 0x10000 - text_end_) & ~Addr{0xFFFF};
    }
    img_ = module_.image;
    orig_text_.assign(img_.begin(), img_.begin() + std::min<Addr>(text_end_, image_len));
    ro_off_ = static_cast<Addr>(module_.segments[1].offset);
    data_off_ = static_cast<Addr>(module_.segments[2].offset);
    if (far_) {
        file_image_len_ = Align(image_len, 0x4000);
        key_page_off_ = Align(image_len + bss_, 0x4000);
        image_size_ = key_page_off_ + 0x4000;
        patch_off_ = -front_;
        // After the key page and the frame step page a main may take
        back_off_ = two_ ? Align(key_page_off_ + 0x8000, 0x10000) : 0;
        if (two_ && back_off_ - 0x8000000 + 0x100000 != back_min_) Fail("BACK_MIN and BACK_OFF disagree");
    } else {
        img_.resize(static_cast<std::size_t>(image_len + bss_));
        key_page_off_ = Align(image_len + bss_, 0x4000);
        img_.resize(img_.size() + 0x4000);
        patch_off_ = Align(static_cast<Addr>(img_.size()), 0x10000);
    }
}

void Rewriter::LoadCuts() {
    // The registered cuts of the build; for a build that registers none, the function-matched patches of the selected
    // profile and those of every game's module of this name (patch_nso.py "Registered patches")
    const std::vector<Cut>* registered = tables_.Cuts(build_id_);
    if (registered != nullptr) {
        cuts_ = *registered;
    } else {
        const std::string& profile = options_.game;
        // The profile's build rows: the builds of this module the game was brought up on, and what they had cut
        struct Known {
            std::string build_id, display;
            std::size_t cuts;
        };
        std::vector<Known> known;
        std::set<std::string> own;
        if (const std::vector<ProfileBuild>* builds = profile.empty() ? nullptr : tables_.Builds(profile)) {
            for (const ProfileBuild& build : *builds) {
                std::string bid;
                for (const auto& [module, id] : build.modules) {
                    if (module == name_) bid = id;  // the last of a name, as a dict keeps it
                }
                if (bid.empty()) continue;
                own.insert(bid);
                const std::vector<Cut>* theirs = tables_.Cuts(bid);
                if (bid != build_id_ && theirs != nullptr && !theirs->empty()) {
                    known.push_back({bid, build.display, theirs->size()});
                }
            }
        }
        std::vector<MatchOutcome> outcomes, shared_outcomes;
        std::vector<Cut> matched =
            Resolve(img_, text_end_, tables_.ProfileMatchCuts(profile, name_), md_, true, &log_, &outcomes);
        cuts_.insert(cuts_.end(), matched.begin(), matched.end());
        matched = Resolve(img_, text_end_, tables_.SharedMatchCuts(name_), md_, false, &log_, &shared_outcomes);
        cuts_.insert(cuts_.end(), matched.begin(), matched.end());
        // A build the game was brought up on lost nothing
        if (!own.contains(build_id_) && (!known.empty() || !outcomes.empty() || !shared_outcomes.empty())) {
            std::string tsv = absl::StrCat(
                "# ", name_, " ", build_id_,
                ": not a registered build. registered: build ID, display version, cuts; patch (the game profile's) or "
                "sdk (SDK-wide): name, cut offsets or -, implementation or why not\n");
            for (const Known& k : known)
                absl::StrAppend(&tsv, "registered\t", k.build_id, "\t", k.display, "\t", k.cuts, "\n");
            for (const auto& [kind, got] : {std::pair("patch", &outcomes), std::pair("sdk", &shared_outcomes)}) {
                for (const MatchOutcome& o : *got) {
                    std::string offsets = "-";
                    if (o.offsets && !o.offsets->empty()) {
                        offsets = absl::StrJoin(*o.offsets, ",", [](std::string* out, Addr a) { out->append(Hex(a)); });
                    }
                    absl::StrAppend(&tsv, kind, "\t", o.name, "\t", offsets, "\t", o.why, "\n");
                }
            }
            match_tsv_ = std::move(tsv);
        }
    }
    const bool main = name_ == "main";
    const bool frame_step = main && tables_.FrameStep(build_id_) != nullptr;
    if (main && !frame_step) {
        if (const std::vector<std::string>* rules = tables_.MatchFrameStep(options_.game, name_)) {
            Fail(
                absl::StrFormat("the profile %s places its frame step by function match (%s), which nxmac's rewriter "
                                "does not do yet: use nxnative's patch_nso.py for this build",
                                options_.game, absl::StrJoin(*rules, "; ")));
        }
    }
    // The frame step state page is the page after the key page. An appended patch area starts on the first 64 KB
    // boundary after the key page, which is that page itself when the key page ends on such a boundary: a main that
    // gets the state page then starts its patch area 64 KB later.
    if (frame_step && !far_ && patch_off_ < key_page_off_ + 0x4000 + 0x1000) patch_off_ += 0x10000;
}

void Rewriter::ApplyCuts() {
    for (const Cut& cut : cuts_) {
        const std::optional<std::vector<std::uint8_t>> ob = HexBytes(cut.original);
        const std::optional<std::vector<std::uint8_t>> nb = HexBytes(cut.replacement);
        const bool sized = ob && nb && ob->size() == nb->size() && cut.offset >= 0 &&
                           static_cast<std::uint64_t>(cut.offset) + ob->size() <= img_.size();
        if (!sized || std::memcmp(img_.data() + cut.offset, ob->data(), ob->size()) != 0) {
            const Addr have = sized ? static_cast<Addr>(ob->size())
                                    : std::clamp<Addr>(static_cast<Addr>(img_.size()) - cut.offset, 0,
                                                       ob ? static_cast<Addr>(ob->size()) : 0);
            Fail(absl::StrFormat(
                "cut original bytes mismatch %s: %s", Hex(cut.offset),
                cut.offset >= 0 ? BytesHex(img_.data() + cut.offset, static_cast<std::size_t>(have)) : ""));
        }
        std::memcpy(img_.data() + cut.offset, nb->data(), nb->size());
    }
}

void Rewriter::WarnEngines() {
    // nxnative finds engine code by what the engine itself holds (its own name strings) and cuts or reports it: the
    // finders are not ported, so a module that shows their marks says so. Its rewrite then lacks those cuts and
    // fixups lines, which nxnative's patch_nso.py would give it.
    const Bytes image(img_);
    const Addr size = std::min<Addr>(static_cast<Addr>(img_.size()), static_cast<Addr>(module_.image.size()));
    std::vector<std::string> marks;
    if (Contains(image, ro_off_, data_off_, Utf16("Binned3") + std::string(2, '\0'))) {
        marks.emplace_back("Unreal's FMallocBinned3 (ue_malloc.py)");
    }
    if (name_ == "main") {
        if (Contains(image, ro_off_, data_off_, "SwitchVirtualAddressSpaceSize")) {
            marks.emplace_back("Unreal's engine address region (ue_region.py)");
        }
        if (Contains(image, ro_off_, data_off_, Utf16("nvn.NumBufferedFrames"))) {
            marks.emplace_back("Unreal's NVN dynamic buffers (ue_ring.py)");
        }
        if (Contains(image, ro_off_, data_off_, Utf16("t.MaxFPS")) ||
            Contains(image, ro_off_, data_off_, "FixedFrameRate")) {
            marks.emplace_back("Unreal's console variables and fixed frame rate (ue_cvar.py, ue_fixed_rate.py)");
        }
        if (Contains(image, 0, size, "UnityEngine.Application::set_targetFrameRate")) {
            marks.emplace_back("Unity's frame rate (unity_fps.py)");
        }
        if (Contains(image, ro_off_, data_off_, "ALLOC_BUCKET")) {
            marks.emplace_back("Unity's bucket allocator (unity_bucket.py)");
        }
    }
    for (const std::string& mark : marks) {
        Log(absl::StrCat("[RED] ", name_, ": the module has ", mark,
                         ", which nxmac's rewriter does not look for yet: this rewrite lacks what nxnative's "
                         "patch_nso.py does there"));
    }
}

void Rewriter::FindFunctions() {
    starts_ = FunctionStarts(img_, text_end_);
    // Data blocks in the text are not code: words there that read as rewrite sites are left alone
    data_blocks_ = DataBlocks(img_, text_end_, md_);
    Addr from = 0;
    for (const auto& [s, e] : data_blocks_) {
        code_ranges_.emplace_back(from, s);
        from = e;
    }
    code_ranges_.emplace_back(from, text_end_);
}

Range Rewriter::FuncOf(Addr a) const {
    // The function holding a: from the FDE start at or before it to the next one. Code before the first FDE is one
    // function as well, from the end of the data block before it (or the text's start) to the first FDE.
    const auto next = std::upper_bound(starts_.begin(), starts_.end(), a);
    if (next != starts_.begin()) return {*(next - 1), next != starts_.end() ? *next : text_end_};
    if (starts_.empty()) return {a, a + 4};
    Addr from = 0;
    for (const auto& [s, e] : data_blocks_) {
        if (e <= a) from = std::max(from, e);
    }
    return {from, starts_.front()};
}

void Rewriter::OrderExclusives() {
    // The exclusive loads and stores without ordering: ldxr / ldxrb / ldxrh / ldxp and stxr / stxrb / stxrh / stxp, any
    // size (each with its fixed fields exact; not CAS or CASP). Bit 15 makes each the same instruction with ordering:
    // ldaxr* / ldaxp (acquire) and stlxr* / stlxp (release). Armv8 lets the accesses after an exclusive load without
    // acquire be satisfied before it, and those before an exclusive store without release be seen after it; the
    // games ran on the Switch's Cortex-A57 without that showing, Apple cores do both. Every one is upgraded: ordering
    // only removes outcomes (exclusive_order_sites).
    for (const auto& [s, e] : code_ranges_) {
        for (Addr o = Align(s, 4); o + 3 < e; o += 4) {
            const std::uint32_t w = Word(o);
            if ((w & 0x3FFFFC00) == 0x085F7C00 || (w & 0xBFFF8000) == 0x887F0000 ||  // ldxr*, ldxp
                (w & 0x3FE0FC00) == 0x08007C00 || (w & 0xBFE08000) == 0x88200000) {  // stxr*, stxp
                ordered_.insert(o);
                PutWord(o, w | 0x8000);
            }
        }
    }
    if (!ordered_.empty()) Stat("exclusive_ordered", static_cast<int>(ordered_.size()));
}

void Rewriter::TrinityCompleter() {
    // The completer of Trinity's per-frame rendezvous (Game Freak):
    //   stp x29,x30,[sp,#-0x20]!; stp x20,x19,[sp,#0x10]; mov x29,sp; ldr x8,[x0,#imm]; mov w9,#1; strb w9,[x8,#A];
    //   ldrb w9,[x8,#A+1]; cbz w9,..; ldr x20,[x8,#B]; add x9,x20,#C; ldar w8,[x9]; cmp w8,#2; b.ne ..
    // It sets the release flag with the plain strb and returns without a wake if the state word (ldar) is 2. A
    // load-acquire does not order an earlier plain store, so the flag can stay invisible while main resets the state,
    // reads the flag as 0 and parks for good. Exactly one such function in a module gets a barrier: the frame
    // pointer's mov gives up its word, the ldr, the mov and the strb move up one and dmb ish follows the strb.
    std::vector<Addr> found;
    for (const auto& [s, e] : code_ranges_) {
        for (Addr o = Align(s, 4); o + 12 <= e; o += 4) {
            if (Word(o) != 0x88DFFD28 || Word(o + 4) != 0x7100091F) continue;  // ldar w8,[x9]; cmp w8,#2
            const Addr f = o - 40;
            if (f < s) continue;
            std::uint32_t w[13];
            for (int i = 0; i < 13; ++i) w[i] = Word(f + 4 * i);
            if (w[0] == 0xA9BE7BFD && w[1] == 0xA9014FF4 && w[2] == 0x910003FD && (w[3] & 0xFFC003FF) == 0xF9400008 &&
                w[4] == 0x52800029 && (w[5] & 0xFFC003FF) == 0x39000109 && (w[6] & 0xFFC003FF) == 0x39400109 &&
                ((w[6] >> 10) & 0xFFF) == ((w[5] >> 10) & 0xFFF) + 1 && (w[7] & 0xFF00001F) == 0x34000009 &&
                (w[8] & 0xFFC003FF) == 0xF9400114 && (w[9] & 0xFFC003FF) == 0x91000289 &&
                (w[12] & 0xFF00001F) == 0x54000001) {
                found.push_back(f);
            }
        }
    }
    if (found.size() > 1) {
        Log(absl::StrFormat(
            "[RED] %s: %d functions have the shape of Trinity's rendezvous completer, so none gets its "
            "barrier: %s",
            name_, found.size(), absl::StrJoin(found, ", ", [](std::string* out, Addr a) { out->append(Hex(a)); })));
    } else if (found.size() == 1) {
        const Addr f = found[0];
        for (int i = 0; i < 3; ++i) PutWord(f + 8 + 4 * i, Word(f + 12 + 4 * i));
        PutWord(f + 20, 0xD5033BBF);  // dmb ish
        Stat("trinity_completer");
    }
}

void Rewriter::ScanSites() {
    // A linear scan for every rewrite point, not relying on Capstone's register lists: the instructions whose operand
    // text names x18 / w18, and the system instructions by their encoding. Spin wait candidates on the way: backward
    // direct branches at most 512 bytes back, by target.
    std::vector<std::pair<Addr, Addr>> spin;  // (head, back-edge)
    std::string op_str;
    for (const auto& [s, e] : code_ranges_) {
        for (Addr o = s; o < e; o += 4) {
            const std::uint32_t w = Word(o);
            const std::uint32_t t = w >> 24;
            if (t == 0x16 || t == 0x17 || t == 0x54 || t == 0x34 || t == 0x35 || t == 0xB4 || t == 0xB5 || t == 0x36 ||
                t == 0x37 || t == 0xB6 || t == 0xB7) {
                Addr d = 4;
                if (t == 0x16 || t == 0x17) {
                    d = (static_cast<Addr>(w & 0x3FFFFFF) - (Addr{1} << 26)) * 4;
                } else if ((t & 0x7E) == 0x36) {
                    if ((w & 0x40000) != 0) d = (static_cast<Addr>((w >> 5) & 0x3FFF) - (Addr{1} << 14)) * 4;
                } else if (t == 0x54) {
                    if ((w & 0x800000) != 0 && (w & 0x10) == 0)
                        d = (static_cast<Addr>((w >> 5) & 0x7FFFF) - (Addr{1} << 19)) * 4;
                } else if ((w & 0x800000) != 0) {
                    d = (static_cast<Addr>((w >> 5) & 0x7FFFF) - (Addr{1} << 19)) * 4;
                }
                if (-512 <= d && d <= 0) spin.emplace_back(o + d, o);
            }
            if ((w & 0xFFE0001F) == 0xD4000001) {
                kinds_[o] = {Kind::kSvc, static_cast<int>((w >> 5) & 0xFFFF)};
            } else if ((w & 0xFFFFFFE0) == 0xD53BD040) {
                kinds_[o] = {Kind::kTpidrR};
            } else if ((w & 0xFFFFFFE0) == 0xD51BD040) {
                kinds_[o] = {Kind::kTpidrW};
            } else if ((w & 0xFFFFFFE0) == 0xD53BD060) {
                kinds_[o] = {Kind::kTpidrroR};
            } else if ((w & 0xFFFFFFE0) == 0xD53BE020) {
                kinds_[o] = {Kind::kCntpct, static_cast<int>(w & 31)};
            } else if ((w & 0xFFFFFFE0) == 0xD53BE040) {
                kinds_[o] = {Kind::kCntvct, static_cast<int>(w & 31)};
            } else if ((w & 0xFFFFFFE0) == 0xD53B0020) {
                // mrs xN, ctr_el0 (cache type): Apple's kernel does not expose it to user mode
                kinds_[o] = {Kind::kCtr, static_cast<int>(w & 31)};
            } else if ((w & 0xFFE0FFFF) == 0xCB2063FF && ((w >> 16) & 31) != 18) {
                // sub sp, sp, xN: sp may end up misaligned; with x18 it is an x18 site
                kinds_[o] = {Kind::kSpAlign, static_cast<int>((w >> 16) & 31)};
            } else if (o + 4 <= e &&
                       ((w & 31) == 18 || ((w >> 5) & 31) == 18 || ((w >> 10) & 31) == 18 || ((w >> 16) & 31) == 18) &&
                       md_.Lite(w, o, &op_str) && Has18(op_str)) {
                // Only a word with 18 in one of the four register fields can print x18 or w18
                kinds_[o] = {Kind::kX18};
            }
        }
    }
    std::stable_sort(spin.begin(), spin.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [head, edge] : spin) {
        if (spin_cand_.empty() || spin_cand_.back().first != head) spin_cand_.emplace_back(head, std::vector<Addr>());
        spin_cand_.back().second.push_back(edge);
    }
}

void Rewriter::EmitSites() {
    for (std::size_t i = 0; i < windows_.size(); ++i) {
        const Addr lo = windows_[i].front().address;
        UseArea(lo);
        const Addr st = SiteWindow(windows_[i]);
        Stat("window");
        PutBranch(lo, st);
    }
    for (const std::vector<RetryPart>& parts : retries_) {
        const Addr lo = parts[0].seg.front().address;
        UseArea(lo);
        const Addr st = SiteRetryWindow(parts);
        Stat("retry_window");
        PutBranch(lo, st);
    }
    for (const auto& [a, site] : kinds_) {
        if (done_.contains(a)) continue;
        UseArea(a);
        const Insn ins = Dis(Word(a), a);
        Addr st = 0;
        switch (site.kind) {
            case Kind::kX18: {
                const std::string& m = ins.mnemonic;
                if (m == "br" && tailcalls_.contains(a)) {
                    st = SiteBrCall();
                    Stat("br_call");
                } else if (m == "br") {
                    if (const auto tab = tabjumps_.find(a); tab != tabjumps_.end()) {
                        st = SiteBrTab(tab->second);
                    } else if (const auto jump = jumps_.find(a); jump != jumps_.end()) {
                        st = SiteBr(jump->second.first, jump->second.second);
                    } else {
                        Fail(absl::StrFormat("%s %s %s: a br through x18 that is no jump of a known form", HexBare(a),
                                             m, ins.op_str));
                    }
                    Stat("br");
                } else if (IsCbTb(ins)) {
                    st = SiteCond(ins);
                    Stat("cond");
                } else if (m == "adr" || m == "adrp") {
                    st = SitePcrel(ins);
                    Stat("pcrel");
                } else if (m == "blr") {
                    st = SiteBlr(ins);
                    Stat("blr");
                } else if (ins.op_str.starts_with("sp,") || ins.op_str.starts_with("wsp,")) {
                    st = SiteSp(ins);
                    Stat("sp");
                } else if (m == "ret" || m == "b" || m == "bl" || (m == "ldr" && !absl::StrContains(ins.op_str, '['))) {
                    Fail(absl::StrFormat("%s %s %s", HexBare(a), m, ins.op_str));
                } else {
                    st = SitePlain(ins);
                    Stat("plain");
                }
                break;
            }
            case Kind::kTpidrR:
                st = SiteSysregRead(ins, Key::kKtls);
                break;
            case Kind::kTpidrroR:
                st = SiteSysregRead(ins, Key::kKtlr);
                break;
            case Kind::kTpidrW:
                st = SiteTpidrWrite(ins);
                break;
            case Kind::kSvc:
            case Kind::kCntpct:
            case Kind::kCntvct:
                st = SiteGate(ins, KindName(site.kind), site.arg);
                break;
            case Kind::kSpAlign:
                st = SiteSpAlign(ins, static_cast<unsigned>(site.arg));
                break;
            case Kind::kCtr:
                // A constant: DminLine = 4 (64-byte lines); movz xN, #4, lsl #16, in place, or into the slot for x18
                if (site.arg != 18) {
                    PutWord(a, 0xD2A00080 | static_cast<std::uint32_t>(site.arg));
                    Stat("ctr");
                    continue;
                }
                st = SiteSlotCopy(ins, std::nullopt, 0xD2A00080 | 15, Key::kK18);
                break;
        }
        if (site.kind != Kind::kX18) Stat(KindName(site.kind));
        PutBranch(a, st);
    }
}

void Rewriter::ObservationGates() {
    const auto free_site = [&](Addr off) { return off < text_end_ && !kinds_.contains(off) && !InWindow(off); };
    // The build's registered gates (OBS): one instruction at an entry is replaced with "enter the host's observation
    // callback, run the original instruction, jump back", so diagnostics change no code at run time
    if (const std::vector<ObservationGate>* registered = tables_.Obs(build_id_)) obs_ = *registered;
    for (const ObservationGate& gate : obs_) {
        const std::uint32_t w = Word(gate.offset);
        if (Hex8(w) != gate.word) {
            Fail(absl::StrFormat("observation gate original instruction mismatch %s: %s", Hex(gate.offset), Hex8(w)));
        }
        if (!free_site(gate.offset)) {
            Fail(absl::StrFormat("observation gate conflicts with a rewrite site %s", Hex(gate.offset)));
        }
        UseArea(gate.offset);
        PutBranch(gate.offset, SiteObs(gate.offset, gate.number, w));
        Stat("obs");
    }
    // Address regions (every module that defines the SDK's VammManager: sdk). Three gates, found by the SDK's own
    // symbols in any build, let the host keep the table of address regions: VammManager::AllocateAddressRegion(this,
    // uintptr_t *out, size_t size) at its entry (the size in x2) and at the one store of the region's address through
    // out (the register in the gate number's upper bits), and VammManager::FreeAddressRegion(this, address) at its
    // entry. A build where one of them is not found that way gets none of them.
    const std::string_view names[] = {kAllocateAddressRegion, kFreeAddressRegion, kStdallocInit, kStdallocInitBool};
    const std::map<std::string, Range, std::less<>> sym = DefinedSymbols(img_, names);
    std::vector<ObservationGate> region;
    const auto ask = sym.find(kAllocateAddressRegion);
    const auto free = sym.find(kFreeAddressRegion);
    // Python's "not ask": a symbol at offset 0 with size 0 is none
    const auto present = [&](const auto& it) { return it != sym.end(); };
    if (present(ask) && present(free)) {
        std::string why_not;
        const OutStore got = FindOutStore(img_, ask->second.first, ask->second.second, md_);
        if (!got.error.empty()) {
            why_not = got.error;
        } else {
            const std::vector<std::tuple<Addr, std::uint32_t, std::string>> sites = {
                {ask->second.first, kObsRegionAsk, "VammManager::AllocateAddressRegion entry: the size in x2"},
                {got.offset, kObsRegionGot | static_cast<std::uint32_t>(got.reg) << 8,
                 absl::StrFormat("VammManager::AllocateAddressRegion: the new region's address in x%d, stored through "
                                 "the out pointer",
                                 got.reg)},
                {free->second.first, kObsRegionFree, "VammManager::FreeAddressRegion entry: the address in x1"}};
            for (const auto& [off, oid, why] : sites) {
                if (!free_site(off) && why_not.empty())
                    why_not = absl::StrFormat("no observation gate possible at %s", Hex(off));
            }
            if (why_not.empty()) {
                for (const auto& [off, oid, why] : sites) region.push_back({off, oid, Hex8(Word(off)), why});
            }
        }
        if (!why_not.empty()) Log(absl::StrFormat("%s: no address region gates: %s", name_, why_not));
    }
    // nn::mem::StandardAllocator::Initialize(this, void *address, size_t size[, bool]): at its entry the host gives a
    // region of virtual address memory (address null) no more address space than the alias region has room for
    for (const std::string_view name : {kStdallocInit, kStdallocInitBool}) {
        const auto it = sym.find(name);
        if (it == sym.end()) continue;
        const Addr off = it->second.first;
        if (!free_site(off)) {
            Log(absl::StrFormat("%s: no StandardAllocator gate at %s: a rewrite site", name_, Hex(off)));
            continue;
        }
        region.push_back({off, kObsStdallocInit, Hex8(Word(off)),
                          absl::StrFormat("StandardAllocator::Initialize(void *, size_t%s) entry: the size in x2, "
                                          "capped when the address in x1 is null",
                                          name.ends_with('b') ? ", bool" : "")});
    }
    for (const ObservationGate& gate : region) {
        UseArea(gate.offset);
        PutBranch(gate.offset,
                  SiteObs(gate.offset, gate.number, static_cast<std::uint32_t>(ParseInt("0x" + gate.word))));
        Stat("obs");
    }
    obs_.insert(obs_.end(), region.begin(), region.end());
}

void Rewriter::FrameStepAndStubs() {
    // Frame step in whole vsyncs (the mains whose patch file has FRAME_STEP of their build): the games step their game
    // time by a whole number of vsyncs per frame that their own interval logic picks; with the stubs the host picks
    // it. The state page is the page after the key page (zero in the file, read-write in both loaders).
    if (name_ == "main") {
        if (const std::string* profile = tables_.FrameStep(build_id_)) {
            const StubFunction stub = FindFrameStep(build_id_);
            if (stub == nullptr) {
                Fail(
                    absl::StrFormat("the frame step of build %s (the profile %s) is code in nxnative's patches.py that "
                                    "nxmac does not have yet (src/rewriter/games): use nxnative's patch_nso.py for it",
                                    build_id_, *profile));
            }
            fs_state_off_ = key_page_off_ + 0x4000;
            // The loader supplies zero-filled bss and key pages for a front patch area: the state page follows them
            if (far_) image_size_ = fs_state_off_ + 0x4000;
            if (fs_state_off_ % 0x1000 != 0 || !(far_ || fs_state_off_ + 0x40 <= patch_off_))
                Fail("frame step state page");
            UseArea(0);
            StubContextImpl ctx(this, "frame step", "frame_step");
            ctx.state = fs_state_off_;
            stub(ctx);
            render_scale_ = ctx.render_scale;
            fs_capabilities_ = ctx.capabilities;
        }
    }
    // Stubs: a fix of the game's own code that needs more instructions than its sites hold
    if (const std::string* profile = tables_.Stubs(build_id_)) {
        const StubFunction stub = FindStubs(build_id_);
        if (stub == nullptr) {
            Fail(
                absl::StrFormat("the stubs of build %s (the profile %s) are code in nxnative's patches.py that nxmac "
                                "does not have yet (src/rewriter/games): use nxnative's patch_nso.py for it",
                                build_id_, *profile));
        }
        UseArea(0);
        StubContextImpl ctx(this, "stub", "stub");
        stub(ctx);
    }
}

std::optional<Addr> Rewriter::Finish(RewriteResult* result) {
    const std::vector<std::uint8_t>& front = patch_front_;
    const Addr back_len = Align(static_cast<Addr>(patch_back_.size()), 0x4000);
    const auto front_len = static_cast<Addr>(front.size());
    std::vector<std::uint8_t>& out = result->patched;
    if (far_) {
        // The patch area before the base: the file is text + ro + data (padded to 16 KB), then the patch area (TWO:
        // the front area padded to 16 KB, then the back area, padded to 16 KB)
        if (big_text_ && front_len > front_ - 0x4000 && !patch_front_given_) return Align(front_len + 0x4000, 0x10000);
        if (front_len > front_ - 0x4000) {
            Fail(absl::StrFormat("patch area of %s bytes does not fit in the %s before the base", Hex(front_len),
                                 Hex(front_)));
        }
        out = img_;
        out.resize(static_cast<std::size_t>(file_image_len_));
        out.insert(out.end(), front.begin(), front.end());
        if (two_) {
            if (image_size_ > back_off_) Fail("back patch area overlaps the key page or the frame step page");
            image_size_ = back_off_ + back_len;
            out.resize(out.size() + static_cast<std::size_t>(-front_len & 0x3FFF));
            out.insert(out.end(), patch_back_.begin(), patch_back_.end());
            out.resize(out.size() + static_cast<std::size_t>(back_len) - patch_back_.size());
        }
    } else {
        out = img_;
        out.resize(static_cast<std::size_t>(patch_off_));
        out.insert(out.end(), front.begin(), front.end());
    }

    std::string& fixups = result->fixups;
    absl::StrAppend(&fixups, "# PATCH_OFF\t", Hex(patch_off_), "\n# KEY_PAGE_OFF\t", Hex(key_page_off_),
                    "\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t", Hex(text_end_), "\n# RO_OFF\t", Hex(ro_off_),
                    "\n# DATA_OFF\t", Hex(data_off_), "\n");
    if (far_)
        absl::StrAppend(&fixups, "# IMAGE_SIZE\t", Hex(image_size_), "\n# FILE_IMAGE_LEN\t", Hex(file_image_len_),
                        "\n");
    if (two_) absl::StrAppend(&fixups, "# BACK_OFF\t", Hex(back_off_), "\n# BACK_LEN\t", Hex(back_len), "\n");
    fixups += "# KEY_K18_IDX\t0\n# KEY_KTLS_IDX\t1\n# KEY_KTLR_IDX\t2\n# KEY_KGATE_IDX\t3\n";
    if (fs_state_off_ != 0) absl::StrAppend(&fixups, "# FPS_STATE_OFF\t", Hex(fs_state_off_), "\n");
    if (render_scale_ != 0) {
        if (render_scale_ < 1 || render_scale_ > 9) {
            Fail(absl::StrFormat("render scale stub: its largest scale is a whole number, 1 to 9: %d", render_scale_));
        }
        absl::StrAppend(&fixups, "# RENDER_SCALE\t", render_scale_, "\n");
    }
    if (name_ == "main") {
        std::set<std::string> capabilities = fs_capabilities_;
        if (const std::vector<std::string>* registered = tables_.Capabilities(build_id_)) {
            capabilities.insert(registered->begin(), registered->end());
        }
        for (const std::string& capability : capabilities) {
            bool valid = !capability.empty() && capability[0] >= 'A' && capability[0] <= 'Z';
            for (const char c : capability)
                valid = valid && ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
            if (!valid) Fail(absl::StrFormat("invalid capability %s", capability));
            absl::StrAppend(&fixups, "# ", capability, "\t1\n");
        }
    }
    // The host identifies the build by it: features with fixed addresses only run on the build they belong to
    absl::StrAppend(&fixups, "# BUILD_ID\t", build_id_, "\n");

    for (const Gate& g : gates_)
        absl::StrAppend(&result->gates, Hex(g.ret), "\t", g.kind, "\t", g.arg, "\t", Hex(g.orig), "\n");

    std::string& sites = result->sites;
    for (const auto& [a, site] : kinds_) {
        if (!done_.contains(a)) absl::StrAppend(&sites, Hex(a), "\t", KindName(site.kind), "\n");
    }
    for (const Range& w : window_ranges_) absl::StrAppend(&sites, Hex(w.first), "\tW\t", Hex(w.second), "\n");
    for (const auto& [a, table] : jumps_)
        absl::StrAppend(&sites, Hex(a), "\tJ\t", Hex(table.first), "\t", table.second, "\n");
    const auto hex_list = [](const auto& list) {
        return absl::StrJoin(list, " ", [](std::string* out, Addr a) { out->append(Hex(a)); });
    };
    // An offset-table jump and all its targets; a spin wait's head and the back-edges that enter its copy
    for (const auto& [a, targets] : tabjumps_) absl::StrAppend(&sites, Hex(a), "\tT\t", hex_list(targets), "\n");
    for (const SpinRegion& g : spin_regions_) absl::StrAppend(&sites, Hex(g.head), "\tS\t", hex_list(g.valid), "\n");

    result->cuts = absl::StrCat("# BUILD_ID\t", build_id_, "\n");
    for (const Cut& c : cuts_)
        absl::StrAppend(&result->cuts, Hex(c.offset), "\t", c.original, "\t", c.replacement, "\t", c.reason, "\n");

    // The text words whose instructions no longer run as the file has them: every word the rewriter changed (sites,
    // cuts, jump tables; not an exclusive access that only gained ordering), the windows whose copies run instead, and
    // the spin waits' instructions, which run from their copies after the first pass. The host's cheat engine refuses
    // a write into any of them.
    std::set<Addr> words;
    for (Addr w = 0; w < text_end_; w += 4) {
        const bool whole = w + 4 <= text_end_;
        if (whole && std::memcmp(img_.data() + w, orig_text_.data() + w, 4) == 0) continue;
        if (whole && ordered_.contains(w) && Word(w) == (U32(orig_text_, w) | 0x8000)) continue;  // ordering alone
        words.insert(w);
    }
    for (const Range& w : window_ranges_) {
        for (Addr a = w.first; a < w.second; a += 4) words.insert(a);
    }
    for (const SpinRegion& g : spin_regions_) {
        for (const Addr a : g.region) {
            const SpinNode& n = g.nodes.at(a);
            for (std::size_t i = 0; i < n.ins.size(); ++i) words.insert(a + 4 * (static_cast<Addr>(i) - n.pre));
        }
    }
    result->changed = absl::StrCat("# BUILD_ID\t", build_id_, "\n");
    for (auto it = words.begin(); it != words.end();) {
        const Addr first = *it;
        Addr length = 0;
        for (; it != words.end() && *it == first + length; ++it) length += 4;
        absl::StrAppend(&result->changed, Hex(first), "\t", Hex(length), "\n");
    }

    result->obs = absl::StrCat("# BUILD_ID\t", build_id_, "\n");
    for (const ObservationGate& o : obs_)
        absl::StrAppend(&result->obs, Hex(o.offset), "\t", o.number, "\t", o.word, "\t", o.reason, "\n");

    result->match = match_tsv_;
    std::string stats = "{";
    for (std::size_t i = 0; i < stats_.size(); ++i) {
        absl::StrAppend(&stats, i == 0 ? "" : ", ", "'", stats_[i].first, "': ", stats_[i].second);
    }
    result->summary = absl::StrCat(name_, " ", stats, "} cuts ", cuts_.size(), " obs ", obs_.size(), " patch ",
                                   patch_front_.size() + patch_back_.size(), " PATCH_OFF ", Hex(patch_off_),
                                   two_ ? absl::StrCat(" BACK_OFF ", Hex(back_off_), " back ", patch_back_.size()) : "",
                                   " slot_refs ", slot_refs_, " gates ", gates_.size());
    if (!data_blocks_.empty()) {
        Log(absl::StrCat(
            name_, " data blocks left alone: ", absl::StrJoin(data_blocks_, ", ", [](std::string* o, const Range& r) {
                absl::StrAppend(o, Hex(r.first), "..", Hex(r.second));
            })));
    }
    result->log = std::move(log_);
    return std::nullopt;
}

std::optional<Addr> Rewriter::Run(RewriteResult* result) {
    LoadCuts();
    result->match = match_tsv_;  // written even when the rewrite stops later
    ApplyCuts();
    WarnEngines();
    patch_base_ = patch_off_;
    FindFunctions();
    OrderExclusives();
    TrinityCompleter();
    ScanSites();
    AnalyseFunctions();
    EmitSites();
    ObservationGates();
    FrameStepAndStubs();
    SpinWaits();
    return Finish(result);
}

}  // namespace internal

absl::StatusOr<RewriteResult> Rewrite(const Module& module, const RewriteOptions& options) {
    internal::Addr patch_front = 0;
    try {
        for (int pass = 0; pass < 2; ++pass) {
            RewriteResult result;
            internal::Rewriter rewriter(module, options, patch_front);
            const std::optional<internal::Addr> need = rewriter.Run(&result);
            if (!need) return result;
            // The patch area did not fit before the base: once more, with exactly the size it needs
            patch_front = *need;
        }
        return absl::InternalError(absl::StrCat(options.name, ": the patch area does not settle on a size"));
    } catch (const internal::RewriteError& e) {
        return absl::FailedPreconditionError(absl::StrCat(options.name, ": ", e.what()));
    } catch (const internal::MatchError& e) {
        return absl::FailedPreconditionError(absl::StrCat(options.name, ": ", e.what()));
    }
}

}  // namespace nxmac::rewriter
