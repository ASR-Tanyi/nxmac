// The rewriter's state and steps (internal to nxmac::rewriter). nxnative's patch_nso.py is one script whose globals
// every step reads; they are this class's members, under the names the script gives them, and each function says which
// of its functions it ports, so the two can be read side by side.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "asm.h"
#include "disasm.h"
#include "image.h"
#include "nxmac/rewriter/rewrite.h"
#include "stubs.h"

namespace nxmac::rewriter::internal {

using InsnList = std::vector<Insn>;

enum class Key { kK18 = 0, kKtls = 1, kKtlr = 2, kKgate = 3 };  // key page indices, fixed

enum class Kind { kSvc, kTpidrR, kTpidrW, kTpidrroR, kCntpct, kCntvct, kCtr, kSpAlign, kX18 };
std::string_view KindName(Kind kind);
struct Site {
    Kind kind;
    int arg = 0;
};

struct Gate {
    Addr ret;
    std::string kind;
    std::int64_t arg;
    Addr orig;
};

// A retry block and the window part it re-enters, each with its fall-through (retry_block, site_retry_window)
struct RetryPart {
    InsnList seg;
    Addr fall_through;
};

// A node of a spin wait region (spin_region1)
struct SpinNode {
    enum Type { kRmw, kAcq, kRel, kTst, kStore, kBranch, kPc, kOk } type = kOk;
    std::string base;  // rmw, acq, rel, tst: the address register
    int k = 0;         // rmw: the constant added; acq 1, rel -1, tst 0
    InsnList ins;
    int pre = 0;  // rel: instructions of the sequence before the node's address
    bool has_use_def = false;
    std::set<std::string> uses;
    std::set<std::string> defs;
    std::vector<Addr> succ;
    std::optional<std::string> store_value;  // store: the value register (none for zr)
    std::string store_base;
};
struct SpinRegion {
    Addr head = 0;
    std::vector<Addr> region;  // sorted
    std::map<Addr, SpinNode> nodes;
    std::set<Addr> valid;  // the back-edges that enter the copy
};

class Rewriter {
public:
    Rewriter(const Module& module, const RewriteOptions& options, Addr patch_front);
    // Runs the whole rewrite. Returns the size the front patch area needs when it did not fit (the caller runs again
    // with it), else nullopt with *result filled.
    std::optional<Addr> Run(RewriteResult* result);

private:
    friend class StubContextImpl;

    // ---- rewriter.cpp: layout, patches, the scan, the order of the steps, the files ----
    void Layout(Addr patch_front);
    void LoadCuts();
    void ApplyCuts();
    void WarnEngines();
    void FindFunctions();
    void OrderExclusives();
    void TrinityCompleter();
    void ScanSites();
    void EmitSites();
    void ObservationGates();
    void FrameStepAndStubs();
    std::optional<Addr> Finish(RewriteResult* result);
    Range FuncOf(Addr a) const;
    bool InWindow(Addr off) const;
    void Stat(std::string_view key, int count = 1);
    void Log(std::string line) { log_.push_back(std::move(line)); }
    std::uint32_t Word(Addr at) const { return U32(img_, at); }
    void PutWord(Addr at, std::uint32_t w) { PutU32(img_, at, w); }
    void PutBranch(Addr at, Addr to) { PutWord(at, a64::B(at, to)); }
    Insn Dis(std::uint32_t w, Addr addr) const;

    // ---- emit.cpp: the patch area and the stubs of each kind of site ----
    void UseArea(Addr a);
    Addr Here() const { return patch_base_ + static_cast<Addr>(patch_->size()); }
    void Emit(std::uint32_t w);
    void EmitKeyAddr(unsigned reg, Key key);
    void EmitSlotAddr(unsigned base, unsigned key_reg, Key key);
    void EmitGateCall();
    std::uint32_t Subst(const Insn& ins, unsigned s) const;
    Addr SitePlain(const Insn& ins);
    Addr SiteSp(const Insn& ins);
    Addr SiteSpAlign(const Insn& ins, unsigned n);
    Addr SiteCond(const Insn& ins);
    Addr SitePcrel(const Insn& ins);
    Addr SiteBr(Addr base, Addr count);
    Addr SiteBrTab(const std::vector<Addr>& targets);
    Addr SiteBlr(const Insn& ins);
    Addr SiteBrCall();
    Addr SiteSlotCopy(const Insn& ins, std::optional<Key> src, std::uint32_t src_word, Key dst);
    Addr SiteSysregRead(const Insn& ins, Key key);
    Addr SiteTpidrWrite(const Insn& ins);
    Addr SiteGate(const Insn& ins, std::string_view kind, std::int64_t arg);
    Addr SiteObs(Addr off, std::uint32_t oid, std::uint32_t word);
    std::uint32_t WindowAdrp(const Insn& x, unsigned s, Addr pc) const;
    void CheckWindowBranches(const InsnList& seg) const;
    Addr SiteWindow(const InsnList& seg);
    Addr SiteRetryWindow(const std::vector<RetryPart>& parts);

    // ---- jumps.cpp: exclusive windows, jump tables and tail calls through x18 ----
    void AnalyseFunctions();
    InsnList DisasmFn(Addr fa, Addr fb) const;
    std::vector<Range> Pieces(Addr fa, Addr fb, std::vector<Range>* far) const;
    static std::vector<InsnList> FindWindows(const InsnList& insl);
    static std::optional<std::pair<InsnList, InsnList>> RetryBlock(const InsnList& insl, std::size_t k,
                                                                   const InsnList& wseg);
    std::vector<Addr> ReadTable(Addr tb, Addr n, Addr base) const;
    std::vector<Addr> JumpOffsets(const InsnList& insl, std::size_t j, bool copy_first, bool far_base,
                                  bool forward_base) const;
    std::vector<Addr> JumpZeroExtended(const InsnList& insl, std::size_t j) const;
    std::optional<Addr> ForwardTableBase(const InsnList& insl, std::size_t k, const std::string& rb, Addr n) const;
    std::vector<Addr> JumpMaskedNot(const InsnList& insl, std::size_t j) const;
    std::vector<Addr> JumpAdrOffsets(const InsnList& insl, std::size_t j) const;
    std::vector<Addr> JumpMaskedPred(const InsnList& insl, std::size_t j) const;
    std::optional<std::pair<Addr, Addr>> JumpBytesFar(const InsnList& insl, std::size_t j) const;
    std::optional<Addr> HalfCount(const InsnList& insl, std::size_t ld, Addr base) const;

    // ---- spin.cpp: spin waits ----
    void SpinWaits();
    const Insn* SpinIns(Addr a);
    bool SpinQuick(Addr head, Addr hi) const;
    std::optional<SpinRegion> FindSpinRegion(Addr head, const std::vector<Addr>& back_edges);
    // A region, or the stores whose paths must be dropped (vary), or neither
    struct SpinTry {
        std::optional<SpinRegion> region;
        std::set<Addr> vary;
    };
    SpinTry SpinRegion1(Addr head, const std::vector<Addr>& back_edges, const std::set<Addr>& ban, bool stores);
    void SpinEmit(const SpinRegion& g);

    // ---- inputs ----
    const Module& module_;
    const RewriteOptions& options_;
    const PatchTables& tables_;
    const std::string name_;  // NAME
    Disassembler md_;

    // ---- layout (the script's upper-case globals) ----
    Addr bss_ = 0;
    bool far_ = false;        // FAR: the patch area lies before the base
    Addr front_ = 0x2000000;  // FRONT
    bool big_text_ = false;   // BIG_TEXT
    bool two_ = false;        // TWO: a second patch area after the image
    Addr back_min_ = 0;
    Addr split_ = 0;  // SPLIT (TWO): sites from here on go to the back area
    bool patch_front_given_ = false;
    std::vector<std::uint8_t> img_;        // IMG
    std::vector<std::uint8_t> orig_text_;  // ORIG_TEXT
    Addr text_end_ = 0, ro_off_ = 0, data_off_ = 0;
    Addr file_image_len_ = 0, key_page_off_ = 0, image_size_ = 0, patch_off_ = 0, back_off_ = 0;
    std::string build_id_;

    // ---- patches ----
    std::vector<Cut> cuts_;
    std::vector<ObservationGate> obs_;
    std::optional<std::string> match_tsv_;
    Addr fs_state_off_ = 0;  // FS_STATE_OFF
    int render_scale_ = 0;   // RENDER_SCALE
    std::set<std::string> fs_capabilities_;

    // ---- the patch area ----
    std::vector<std::uint8_t> patch_front_, patch_back_;
    std::vector<std::uint8_t>* patch_ = &patch_front_;
    Addr patch_base_ = 0;  // PATCH_BASE
    std::size_t slot_refs_ = 0;
    std::vector<Gate> gates_;
    std::vector<std::pair<std::string, int>> stats_;

    // ---- what the scan found ----
    std::vector<Addr> starts_;           // function starts, sorted
    std::vector<Range> data_blocks_;     // DATA_BLOCKS
    std::vector<Range> code_ranges_;     // CODE_RANGES
    absl::flat_hash_set<Addr> ordered_;  // ORDERED
    std::map<Addr, Site> kinds_;
    std::vector<std::pair<Addr, std::vector<Addr>>> spin_cand_;  // by head, sorted
    absl::flat_hash_set<Addr> done_;
    std::vector<InsnList> windows_;
    std::vector<Range> window_ranges_;             // [first, last + 4) of each window
    std::map<Addr, std::pair<Addr, Addr>> jumps_;  // br x18 -> (base, width)
    std::map<Addr, std::vector<Addr>> tabjumps_;   // br x18 -> its targets
    absl::flat_hash_set<Addr> tailcalls_;
    std::vector<std::vector<RetryPart>> retries_;

    // ---- spin waits ----
    std::unordered_map<Addr, std::optional<Insn>> spin_dec_;  // pointers into it stay valid as it grows
    absl::flat_hash_set<Addr> spin_touched_;
    std::vector<SpinRegion> spin_regions_;

    std::vector<std::string> log_;
};

}  // namespace nxmac::rewriter::internal
