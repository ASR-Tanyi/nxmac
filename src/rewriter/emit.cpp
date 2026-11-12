// The patch area and the stub of each kind of rewrite site (patch_nso.py "Rewrite blocks of each kind" and "Exclusive
// windows"). Every stub ends in a branch back to the instruction after its site.
#include <algorithm>

#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "rewriter_impl.h"
#include "text.h"

namespace nxmac::rewriter::internal {
namespace {

using namespace a64;  // NOLINT: the encoders, as the stubs read

// The registers an instruction names or Capstone lists for it, as a bit per number (regs_in)
std::uint32_t RegsIn(const Insn& ins) {
    std::uint32_t mask = 0;
    for (const int n : RegsNamed(ins.op_str)) {
        if (n < 32) mask |= 1U << n;
    }
    if (ins.regs_ok) {
        for (const auto* list : {&ins.regs_read, &ins.regs_write}) {
            for (const std::uint16_t reg : *list) {
                if (const std::optional<int> n = RegNum(ins.Name(reg)); n && *n < 32) mask |= 1U << *n;
            }
        }
    }
    return mask;
}

// The first n registers of x0..x15 that avoid does not hold (pick)
std::vector<unsigned> Pick(std::uint32_t avoid, std::size_t n = 2) {
    std::vector<unsigned> out;
    for (unsigned r = 0; r < 16 && out.size() < n; ++r) {
        if ((avoid >> r & 1) == 0) out.push_back(r);
    }
    if (out.size() != n) Fail("no scratch register is free at a rewrite site");
    return out;
}

// A number as the pattern -?(?:0x)?[0-9a-f]+ takes it from the start of text; its length
std::size_t NumberLength(std::string_view text) {
    std::size_t at = 0;
    if (at < text.size() && text[at] == '-') ++at;
    if (at + 1 < text.size() && text[at] == '0' && text[at + 1] == 'x') at += 2;
    const std::size_t digits = at;
    while (at < text.size() && ((text[at] >= '0' && text[at] <= '9') || (text[at] >= 'a' && text[at] <= 'f'))) ++at;
    return at == digits ? 0 : at;
}

// What an instruction writes back to sp (pre-index [sp, #i]! / post-index [sp], #i); 0 when it does not (sp_delta)
std::int64_t SpDelta(const Insn& ins) {
    const std::string_view op = ins.op_str;
    for (std::size_t at = op.find("[sp, #"); at != std::string_view::npos; at = op.find("[sp, #", at + 1)) {
        const std::string_view rest = op.substr(at + 6);
        const std::size_t length = NumberLength(rest);
        if (length != 0 && rest.substr(length, 2) == "]!") return ParseInt(rest.substr(0, length));
    }
    for (std::size_t at = op.find("[sp], #"); at != std::string_view::npos; at = op.find("[sp], #", at + 1)) {
        const std::string_view rest = op.substr(at + 7);
        if (const std::size_t length = NumberLength(rest); length != 0 && length == rest.size()) return ParseInt(rest);
    }
    if (op.starts_with("sp,") || op.starts_with("wsp,")) {
        Fail(absl::StrFormat("writes sp: %s %s", HexBare(ins.address), ins.op_str));
    }
    return 0;
}

bool IsDirectBranch(const Insn& x) { return x.Is("b") || IsCbTb(x) || IsBCond(x); }

// A window's instruction that cannot be copied: pc-relative, a call or a return
void CheckRelocatable(const Insn& x) {
    if (x.IsAny("adr", "adrp", "b", "bl", "br", "blr", "ret") || (x.Is("ldr") && !absl::StrContains(x.op_str, '['))) {
        Fail(absl::StrFormat("an exclusive sequence holds an instruction that cannot be copied at %s", Hex(x.address)));
    }
}

}  // namespace

void Rewriter::UseArea(Addr a) {
    // Emit what follows into the patch area that reaches the code at offset a (TWO: the back area from SPLIT on)
    if (two_ && a >= split_) {
        patch_ = &patch_back_;
        patch_base_ = back_off_;
    } else {
        patch_ = &patch_front_;
        patch_base_ = patch_off_;
    }
}

void Rewriter::Emit(std::uint32_t w) {
    const std::size_t at = patch_->size();
    patch_->resize(at + 4);
    std::memcpy(patch_->data() + at, &w, 4);
}

void Rewriter::EmitKeyAddr(unsigned reg, Key key) {
    ++slot_refs_;
    Emit(Adrp(reg, Here(), key_page_off_));
    Emit(LdrX(reg, reg, static_cast<unsigned>(key)));
}

void Rewriter::EmitSlotAddr(unsigned base, unsigned key_reg, Key key) {
    EmitKeyAddr(key_reg, key);
    Emit(AddR(base, base, key_reg));
}

std::uint32_t Rewriter::Subst(const Insn& ins, unsigned s) const {
    // x18 / w18 -> xS / wS: only the 4 register field positions are changed, verified against the disassembly text
    const std::uint32_t w = ins.word;
    const std::string want = Subst18(ins.op_str, static_cast<int>(s));
    std::vector<int> pos;
    for (const int p : {0, 5, 10, 16}) {
        if (((w >> p) & 31) == 18) pos.push_back(p);
    }
    for (std::size_t k = pos.size(); k > 0; --k) {
        std::vector<bool> take(pos.size(), false);
        std::fill(take.begin(), take.begin() + static_cast<std::ptrdiff_t>(k), true);
        do {  // the combinations of k positions, in order
            std::uint32_t nw = w;
            for (std::size_t i = 0; i < pos.size(); ++i) {
                if (take[i]) nw = (nw & ~(31U << pos[i])) | (s << pos[i]);
            }
            const std::optional<Insn> dd = md_.One(nw, ins.address);
            if (dd && dd->mnemonic == ins.mnemonic && dd->op_str == want) return nw;
        } while (std::prev_permutation(take.begin(), take.end()));
    }
    Fail(absl::StrFormat("subst fail %s %s %s", HexBare(ins.address), ins.mnemonic, ins.op_str));
}

Addr Rewriter::SitePlain(const Insn& ins) {
    const std::vector<unsigned> st = Pick(RegsIn(ins) | 1U << 18);
    const unsigned s = st[0], t = st[1];
    const std::int64_t delta = SpDelta(ins);
    // Instructions that modify sp write below sp, so the scratch save is placed deep in the reserved area
    const int save = delta == 0 ? -16 : -112;
    if (delta < -96) Fail(absl::StrFormat("sp delta %d at %s", delta, HexBare(ins.address)));
    const Addr start = Here();
    Emit(Stp(s, t, 31, save));
    Emit(MrsTpidrro(t));
    EmitSlotAddr(t, s, Key::kK18);
    Emit(LdrX(s, t));
    Emit(Subst(ins, s));
    Emit(StrX(s, t));  // always write back: Capstone can miss reporting a write
    Emit(Ldp(s, t, 31, save - static_cast<int>(delta)));
    Emit(B(Here(), ins.address + 4));
    return start;
}

Addr Rewriter::SiteSp(const Insn& ins) {
    // The destination is sp and a source is x18 (an alloca: mov x18, sp; sub x18, x18, x0; mov sp, x18). site_plain
    // saves its scratch relative to the old sp and would restore it relative to the new one: the old sp is copied into
    // T first, and S / T are restored relative to T. x18 is not written, so the slot is not written back.
    const std::vector<unsigned> st = Pick(RegsIn(ins) | 1U << 18);
    const unsigned s = st[0], t = st[1];
    const Addr start = Here();
    Emit(Stp(s, t, 31, -16));
    Emit(MrsTpidrro(t));
    EmitSlotAddr(t, s, Key::kK18);
    Emit(LdrX(s, t));
    Emit(AddI(t, 31, 0));  // mov T, sp
    Emit(Subst(ins, s));
    Emit(Ldp(s, t, t, -16));
    Emit(B(Here(), ins.address + 4));
    return start;
}

Addr Rewriter::SiteSpAlign(const Insn& ins, unsigned n) {
    // sub sp, sp, xN: xN is rounded up to 16 first, which the matching add then gives back too
    const Addr start = Here();
    Emit(AddI(n, n, 15));
    Emit(0x927CEC00 | n << 5 | n);  // and xN, xN, #~15
    Emit(ins.word);
    Emit(B(Here(), ins.address + 4));
    return start;
}

Addr Rewriter::SiteCond(const Insn& ins) {
    const std::uint32_t w = ins.word;
    const std::vector<unsigned> st = Pick(RegsIn(ins) | 1U << 18);
    const unsigned s = st[0], t = st[1];
    const Addr target = ins.Target();
    const Addr start = Here();
    Emit(Stp(s, t, 31, -16));
    Emit(MrsTpidrro(t));
    EmitSlotAddr(t, s, Key::kK18);
    Emit(LdrX(s, t));
    const Addr taken = Here() + 4 * 3;
    const auto off = static_cast<std::uint32_t>((taken - Here()) >> 2);
    const std::uint32_t nw = ins.IsAny("cbz", "cbnz") ? ((w & 0xFF000000) | (off & 0x7FFFF) << 5 | s)
                                                      : ((w & 0xFFF80000) | (off & 0x3FFF) << 5 | s);
    const Addr pc = Here();
    Emit(nw);
    const Insn dd = Dis(nw, pc);
    if (dd.mnemonic != ins.mnemonic || dd.Target() != taken) Fail(absl::StrFormat("cond site %s", Hex(ins.address)));
    Emit(Ldp(s, t, 31, -16));
    Emit(B(Here(), ins.address + 4));
    if (Here() != taken) Fail("cond site layout");
    Emit(Ldp(s, t, 31, -16));
    Emit(B(Here(), target));
    return start;
}

Addr Rewriter::SitePcrel(const Insn& ins) {
    const Addr target = ins.operands[1].imm;
    const unsigned s = 0, t = 1;
    const Addr start = Here();
    Emit(Stp(s, t, 31, -16));
    Emit(MrsTpidrro(t));
    EmitSlotAddr(t, s, Key::kK18);  // S is first the key page offset's temporary; the next instruction loads the target
    const Addr pc = Here();
    Emit(Adrp(s, pc, target));
    if (ins.Is("adr")) Emit(AddI(s, s, static_cast<unsigned>(target & 0xFFF)));
    Emit(StrX(s, t));
    Emit(Ldp(s, t, 31, -16));
    Emit(B(Here(), ins.address + 4));
    return start;
}

Addr Rewriter::SiteBr(Addr base, Addr count) {
    // A jump table's br x18: target = base + 4i. x16 goes to the red zone, then equally spaced landing pads, each
    // restoring x16 and going to the original target.
    const Addr start = Here();
    Emit(Stur(16, 31, -16));
    Emit(Stur(17, 31, -24));  // x16 / x17 can be live inside a function: saved in the red zone before borrowing
    Emit(MrsTpidrro(16));
    EmitSlotAddr(16, 17, Key::kK18);
    Emit(LdrX(16, 16));
    Emit(Ldur(17, 31, -24));
    const Addr padbase = start + 4 * 24;
    const Addr delta = padbase - base;
    const auto op = delta >= 0 ? AddI : SubI;
    const Addr dl = delta >= 0 ? delta : -delta;
    std::vector<std::uint32_t> ops;
    Addr hi = dl >> 12;
    const Addr lo = dl & 0xFFF;
    while (hi != 0) {
        const Addr h = std::min<Addr>(hi, 0xFFF);
        ops.push_back(op(16, 16, static_cast<unsigned>(h), 1));
        hi -= h;
    }
    if (lo != 0) ops.push_back(op(16, 16, static_cast<unsigned>(lo), 0));
    if (ops.size() > 12) Fail(absl::StrFormat("jump table at %s is too far from its landing pads", Hex(base)));
    for (const std::uint32_t o : ops) Emit(o);
    Emit(Br(16));
    while (Here() < padbase) Emit(kNop);
    const Addr tails = padbase + 4 * count;
    for (Addr i = 0; i < count; ++i) Emit(B(Here(), tails + 8 * i));
    for (Addr i = 0; i < count; ++i) {
        Emit(Ldur(16, 31, -16));
        Emit(B(Here(), base + 4 * i));
    }
    return start;
}

Addr Rewriter::SiteBrTab(const std::vector<Addr>& targets) {
    // An offset table's br x18: the table is constant, so its targets were read at rewrite time. x16 = the guest's
    // x18, compared with each target in turn; on a match x16 / x17 are restored and the original target is taken; no
    // match is a brk (the original bounds check already stops indices out of range).
    const Addr start = Here();
    Emit(Stur(16, 31, -16));
    Emit(Stur(17, 31, -24));
    Emit(MrsTpidrro(16));
    EmitSlotAddr(16, 17, Key::kK18);
    Emit(LdrX(16, 16));
    std::vector<Addr> uniq = targets;
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    const Addr pads = Here() + 4 * (4 * static_cast<Addr>(uniq.size()) + 1);
    for (std::size_t k = 0; k < uniq.size(); ++k) {
        const Addr t = uniq[k];
        Emit(Adrp(17, Here(), t));
        Emit(AddI(17, 17, static_cast<unsigned>(t & 0xFFF)));
        Emit(CmpR(16, 17));
        Emit(Beq(Here(), pads + 12 * static_cast<Addr>(k)));
    }
    Emit(Brk(0x18));
    for (const Addr t : uniq) {
        Emit(Ldur(16, 31, -16));
        Emit(Ldur(17, 31, -24));
        Emit(B(Here(), t));
    }
    return start;
}

Addr Rewriter::SiteBlr(const Insn& ins) {
    // blr x18: x16 = the guest's x18, x30 = the instruction after the original blr, br x16. At a call x16 / x17 are
    // free, and the return address is the original one (C++ exceptions look up unwind tables by return address).
    const Addr start = Here();
    Emit(MrsTpidrro(16));
    EmitSlotAddr(16, 17, Key::kK18);
    Emit(LdrX(16, 16));
    const Addr ret = ins.address + 4;
    Emit(Adrp(30, Here(), ret));
    Emit(AddI(30, 30, static_cast<unsigned>(ret & 0xFFF)));
    Emit(Br(16));
    return start;
}

Addr Rewriter::SiteBrCall() {
    // br x18 as an indirect tail call: x16 = the guest's x18, br x16; x30 keeps the caller's return address
    const Addr start = Here();
    Emit(MrsTpidrro(16));
    EmitSlotAddr(16, 17, Key::kK18);
    Emit(LdrX(16, 16));
    Emit(Br(16));
    return start;
}

Addr Rewriter::SiteSlotCopy(const Insn& ins, std::optional<Key> src, std::uint32_t src_word, Key dst) {
    // A system register access whose general register is x18: a copy from one thread slot to another, or src_word
    // puts the value in x15 (mrs x18, ctr_el0). The kernel may change x18 at any instruction, so the value never
    // passes through it: x15 carries it, x16 / x17 form the slot addresses, all three saved in the red zone.
    const Addr start = Here();
    Emit(Stp(16, 17, 31, -16));
    Emit(Stur(15, 31, -24));
    if (src) {
        Emit(MrsTpidrro(16));
        EmitSlotAddr(16, 17, *src);
        Emit(LdrX(15, 16));
    } else {
        Emit(src_word);
    }
    Emit(MrsTpidrro(16));
    EmitSlotAddr(16, 17, dst);
    Emit(StrX(15, 16));
    Emit(Ldur(15, 31, -24));
    Emit(Ldp(16, 17, 31, -16));
    Emit(B(Here(), ins.address + 4));
    return start;
}

Addr Rewriter::SiteSysregRead(const Insn& ins, Key key) {
    // mrs xN, tpidr_el0 / tpidrro_el0 -> xN = the thread slot's value
    const unsigned n = ins.word & 31;
    if (n == 18) return SiteSlotCopy(ins, key, 0, Key::kK18);
    const unsigned tmp = n != 16 ? 16 : 17;
    const Addr start = Here();
    Emit(Stur(tmp, 31, -16));  // tmp is saved in the red zone
    Emit(MrsTpidrro(n));
    EmitSlotAddr(n, tmp, key);
    Emit(LdrX(n, n));
    Emit(Ldur(tmp, 31, -16));
    Emit(B(Here(), ins.address + 4));
    return start;
}

Addr Rewriter::SiteTpidrWrite(const Insn& ins) {
    const unsigned n = ins.word & 31;
    if (n == 18) return SiteSlotCopy(ins, Key::kK18, 0, Key::kKtls);
    const unsigned t = Pick(1U << n | 1U << 18, 1)[0];
    const Addr start = Here();
    Emit(Stur(t, 31, -16));
    const unsigned tmp = (n != 16 && t != 16) ? 16 : 17;
    Emit(Stur(tmp, 31, -24));
    Emit(MrsTpidrro(t));
    EmitSlotAddr(t, tmp, Key::kKtls);
    Emit(StrX(n, t));
    Emit(Ldur(tmp, 31, -24));
    Emit(Ldur(t, 31, -16));
    Emit(B(Here(), ins.address + 4));
    return start;
}

void Rewriter::EmitGateCall() {
    // The common part of a gate block, after stp x29,x30,[sp,#-16]! and before blr x30: x30 = the gate address in
    // thread slot KGATE. x29 is borrowed for the key page and restored from the stack at once: the x29 the gate saves
    // must be the guest's, since the crash backtrace walks the x29 chain.
    Emit(MrsTpidrro(30));
    EmitSlotAddr(30, 29, Key::kKgate);
    Emit(LdrX(30, 30));
    Emit(LdrX(29, 31));
}

Addr Rewriter::SiteGate(const Insn& ins, std::string_view kind, std::int64_t arg) {
    // svc / mrs cntpct_el0 -> the host gate, which looks up gates.tsv by return point (the address of this block's ldp)
    const Addr start = Here();
    Emit(StpPre(29, 30, 31, -16));
    EmitGateCall();
    Emit(Blr(30));
    const Addr ret = Here();
    Emit(LdpPost(29, 30, 31, 16));
    Emit(B(Here(), ins.address + 4));
    gates_.push_back({ret, std::string(kind), arg, ins.address});
    return start;
}

Addr Rewriter::SiteObs(Addr off, std::uint32_t oid, std::uint32_t word) {
    // An observation gate: the original instruction -> enter the host gate (arg = the observation number) -> restore
    // x29 / x30 -> run the original instruction -> back to off + 4. Any instruction that can be relocated takes one.
    const Insn ins = Dis(word, off);
    const bool fixed = !ins.IsAny("adr", "adrp", "b", "bl", "br", "blr", "ret", "cbz", "cbnz", "tbz", "tbnz") &&
                       !IsBCond(ins) && !(ins.mnemonic.starts_with("ldr") && !absl::StrContains(ins.op_str, '[')) &&
                       !Has18(ins.op_str);
    if (!fixed) {
        Fail(absl::StrFormat("observation gate original instruction cannot be relocated %s %s %s", Hex(off),
                             ins.mnemonic, ins.op_str));
    }
    const Addr start = Here();
    Emit(StpPre(29, 30, 31, -16));
    EmitGateCall();
    Emit(Blr(30));
    const Addr ret = Here();
    Emit(LdpPost(29, 30, 31, 16));
    Emit(word);
    Emit(B(Here(), off + 4));
    gates_.push_back({ret, "obs", oid, off});
    return start;
}

std::uint32_t Rewriter::WindowAdrp(const Insn& x, unsigned s, Addr pc) const {
    // An adrp inside a window, copied to pc: the same page from the copy's address (x18 as its stand-in S)
    const std::optional<int> rd = RegNum(x.OpReg(0));
    if (!rd) Fail(absl::StrFormat("adrp into %s in an exclusive sequence at %s", x.OpReg(0), Hex(x.address)));
    return Adrp(*rd == 18 ? s : static_cast<unsigned>(*rd), pc, x.operands[1].imm);
}

void Rewriter::CheckWindowBranches(const InsnList& seg) const {
    // For windows with a b: no exit may carry a reservation through a stub's store
    if (std::none_of(seg.begin(), seg.end(), [](const Insn& x) { return x.Is("b"); })) return;
    for (std::size_t i = 1; i < seg.size(); ++i) {
        if (seg[i].address != seg[i - 1].address + 4)
            Fail(absl::StrFormat("noncontiguous window %s", HexBare(seg[0].address)));
    }
    const Addr lo = seg[0].address;
    const auto in_window = [&](Addr a) {
        return a >= lo && a < lo + 4 * static_cast<Addr>(seg.size()) && (a - lo) % 4 == 0;
    };
    std::vector<std::pair<Addr, bool>> pending{{lo, false}};
    std::set<std::pair<Addr, bool>> seen;
    while (!pending.empty()) {
        auto [addr, live] = pending.back();
        pending.pop_back();
        if (!in_window(addr)) {
            if (live) Fail(absl::StrFormat("branch leaves window with live reservation %s", HexBare(addr)));
            continue;
        }
        if (!seen.emplace(addr, live).second) continue;
        const Insn& x = seg[static_cast<std::size_t>((addr - lo) / 4)];
        if (x.IsAny("ldxr", "ldaxr", "ldxp", "ldaxp", "ldxrb", "ldaxrb", "ldxrh", "ldaxrh")) {
            live = true;
        } else if (x.IsAny("stxr", "stlxr", "stxp", "stlxp", "stxrb", "stlxrb", "stxrh", "stlxrh", "clrex")) {
            live = false;
        }
        if (x.Is("b")) {
            pending.emplace_back(x.Target(), live);
        } else if (IsCbTb(x) || IsBCond(x)) {
            pending.emplace_back(x.Target(), live);
            pending.emplace_back(addr + 4, live);
        } else {
            pending.emplace_back(addr + 4, live);
        }
    }
}

Addr Rewriter::SiteWindow(const InsnList& seg) {
    CheckWindowBranches(seg);
    std::uint32_t used = 1U << 18;
    for (const Insn& x : seg) used |= RegsIn(x);
    const std::vector<unsigned> st = Pick(used);
    const unsigned s = st[0], t = st[1];
    const Addr lo = seg.front().address;
    const Addr hi = seg.back().address + 4;
    const Addr start = Here();
    Emit(Stp(s, t, 31, -16));
    Emit(MrsTpidrro(t));
    EmitSlotAddr(t, s, Key::kK18);
    Emit(LdrX(s, t));
    const Addr copy0 = Here();
    const auto n = static_cast<Addr>(seg.size());
    // Exit stubs: the first one after the window is the fall-through (after the last window instruction, hi); the
    // targets of branches that leave the window follow it. A successful store must not fall into a branch-out stub.
    const Addr exits = copy0 + 4 * n + 16;
    std::vector<Addr> ex_targets;
    std::vector<std::uint32_t> words;
    for (std::size_t k = 0; k < seg.size(); ++k) {
        const Insn& x = seg[k];
        std::uint32_t w = Has18(x.op_str) ? Subst(x, s) : x.word;
        if (IsDirectBranch(x)) {
            const Addr target = x.Target();
            Addr newt;
            if (lo <= target && target < hi) {
                newt = copy0 + (target - lo);
            } else {
                ex_targets.push_back(target);
                newt = exits + 16 * (static_cast<Addr>(ex_targets.size()) - 1);
            }
            const Addr pc = copy0 + 4 * static_cast<Addr>(k);
            const auto off = static_cast<std::uint32_t>((newt - pc) >> 2);
            if (x.Is("b")) {
                w = B(pc, newt);
            } else if (x.IsAny("tbz", "tbnz")) {
                w = (w & 0xFFF8001F) | (off & 0x3FFF) << 5;
            } else {
                w = (w & 0xFF00001F) | (off & 0x7FFFF) << 5;
            }
            const Insn dd = Dis(w, pc);
            if (dd.Target() != newt || dd.mnemonic != x.mnemonic)
                Fail(absl::StrFormat("window branch %s", Hex(x.address)));
        } else if (x.Is("adrp")) {
            w = WindowAdrp(x, s, copy0 + 4 * static_cast<Addr>(k));
        } else {
            CheckRelocatable(x);
        }
        words.push_back(w);
    }
    for (const std::uint32_t w : words) Emit(w);
    std::vector<Addr> outs{hi};
    outs.insert(outs.end(), ex_targets.begin(), ex_targets.end());
    for (const Addr target : outs) {  // fall-through first
        Emit(StrX(s, t));
        Emit(Ldp(s, t, 31, -16));
        Emit(B(Here(), target));
        Emit(kNop);
    }
    return start;
}

Addr Rewriter::SiteRetryWindow(const std::vector<RetryPart>& parts) {
    // A retry block gets a patch copy of its own: it cannot jump into the window's copy, because the x18 stand-in
    // would have to be loaded between its exclusive load and the store. The parts are copied one after the other under
    // one prologue, each followed by its fall-through stub; branches to any copied instruction stay inside the copy,
    // every other target leaves through an exit stub as in SiteWindow.
    std::uint32_t used = 1U << 18;
    for (const RetryPart& part : parts) {
        for (const Insn& x : part.seg) used |= RegsIn(x);
    }
    const std::vector<unsigned> st = Pick(used);
    const unsigned s = st[0], t = st[1];
    const Addr start = Here();
    Emit(Stp(s, t, 31, -16));
    Emit(MrsTpidrro(t));
    EmitSlotAddr(t, s, Key::kK18);
    Emit(LdrX(s, t));
    std::map<Addr, Addr> pos;
    Addr at = Here();
    for (const RetryPart& part : parts) {
        for (const Insn& x : part.seg) {
            pos[x.address] = at;
            at += 4;
        }
        at += 16;
    }
    const Addr exits = at;
    std::vector<Addr> ex_targets;
    for (const RetryPart& part : parts) {
        for (const Insn& x : part.seg) {
            std::uint32_t w = Has18(x.op_str) ? Subst(x, s) : x.word;
            const Addr pc = Here();
            if (IsCbTb(x) || IsBCond(x)) {
                const Addr target = x.Target();
                const auto copied = pos.find(target);
                auto exit = std::find(ex_targets.begin(), ex_targets.end(), target);
                if (copied == pos.end() && exit == ex_targets.end()) {
                    ex_targets.push_back(target);
                    exit = ex_targets.end() - 1;
                }
                const Addr newt = copied != pos.end() ? copied->second : exits + 16 * (exit - ex_targets.begin());
                const auto off = static_cast<std::uint32_t>((newt - pc) >> 2);
                if (x.IsAny("tbz", "tbnz")) {
                    w = (w & 0xFFF8001F) | (off & 0x3FFF) << 5;
                } else {
                    w = (w & 0xFF00001F) | (off & 0x7FFFF) << 5;
                }
                const Insn dd = Dis(w, pc);
                if (dd.Target() != newt || dd.mnemonic != x.mnemonic)
                    Fail(absl::StrFormat("retry branch %s", Hex(x.address)));
            } else if (x.Is("adrp")) {
                w = WindowAdrp(x, s, pc);
            } else {
                CheckRelocatable(x);
            }
            Emit(w);
        }
        Emit(StrX(s, t));
        Emit(Ldp(s, t, 31, -16));
        Emit(B(Here(), part.fall_through));
        Emit(kNop);
    }
    for (const Addr target : ex_targets) {
        Emit(StrX(s, t));
        Emit(Ldp(s, t, 31, -16));
        Emit(B(Here(), target));
        Emit(kNop);
    }
    return start;
}

}  // namespace nxmac::rewriter::internal
