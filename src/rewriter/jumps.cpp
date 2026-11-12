// What the rewriter reads from a function before it rewrites x18 in it (patch_nso.py "Exclusive windows", and the jump
// table and tail call recognizers): the exclusive sequences that use x18, which are copied whole; where each br x18
// goes, as a jump table of one of the forms compilers make or as an indirect tail call; and the branches that may not
// enter a copied sequence. A br x18 that fits no form stops the rewrite: a wrong guess would run wild in the game.
#include <algorithm>
#include <cstdlib>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "rewriter_impl.h"
#include "text.h"

namespace nxmac::rewriter::internal {
namespace {

using Index = std::ptrdiff_t;

constexpr Addr kBigFn = 0x40000;   // a function longer than this is analysed in pieces
constexpr Addr kPiece = 0x40000;   // of about this size
constexpr Addr kPieceB = 0x10000;  // a b farther than this may cross a cut

bool IsExclusiveLoad(const Insn& x) {
    return x.IsAny("ldxr", "ldaxr", "ldxp", "ldaxp", "ldxrb", "ldaxrb", "ldxrh", "ldaxrh");
}
bool IsExclusiveStore(const Insn& x) {
    return x.IsAny("stxr", "stlxr", "stxp", "stlxp", "stxrb", "stlxrb", "stxrh", "stlxrh");
}
// b, bl, cbz / cbnz, tbz / tbnz, b.cond: a branch with its target in the instruction
bool IsDirect(const Insn& x) { return x.IsAny("b", "bl") || IsCbTb(x) || IsBCond(x); }
// The same without bl
bool IsJump(const Insn& x) { return x.Is("b") || IsCbTb(x) || IsBCond(x); }
// Any instruction that leaves the straight line
bool IsControlFlow(const Insn& x) { return IsJump(x) || x.IsAny("br", "blr", "bl", "ret"); }

std::string_view Canon(std::string_view name) { return name == "lr" ? "x30" : name == "fp" ? "x29" : name; }

// The number of a register by Capstone's name, fp and lr included
std::optional<int> NumOf(std::string_view name) { return RegNum(Canon(name)); }

// The number of a register a pattern took as x<digits> or w<digits>
int NumberOf(std::string_view name) {
    const std::optional<int> n = RegNum(name);
    if (!n) Fail(absl::StrCat("no general register: ", name));
    return *n;
}

std::uint32_t Bit(int n) { return n >= 0 && n < 32 ? 1U << n : 0; }
std::uint32_t BitOf(std::string_view name) {
    const std::optional<int> n = RegNum(name);
    return n ? Bit(*n) : 0;
}

// The number after the first # of the operand text (int(op_str.split('#')[1], 0))
Addr ImmAfterHash(const Insn& p) {
    std::string_view rest = std::string_view(p.op_str).substr(p.op_str.find('#') + 1);
    rest = rest.substr(0, rest.find('#'));
    return ParseInt(rest);
}

bool Contiguous(const InsnList& l, Index from, Index to) {  // l[from..to] follow one another
    for (Index i = from; i < to; ++i) {
        if (l[i + 1].address != l[i].address + 4) return false;
    }
    return true;
}

// Whether p sets a table base to a constant inside page zero, so that a table load of n entries through it reads that
// page. Horizon never maps page zero: no path of a working game carries such a write to the load, and a walk back for
// the base's writer chain goes past it (null_base).
bool NullBase(const Insn& p, Addr n) {
    if (!p.Is("mov") || p.op_count != 2) return false;
    std::optional<Addr> v;
    if (p.op_str.ends_with("wzr") || p.op_str.ends_with("xzr")) {
        v = 0;
    } else if (p.operands[1].type == ARM64_OP_IMM) {
        v = p.operands[1].imm;
    }
    return v && 0 <= *v && *v + 4 * n <= 0x1000;
}

// How many entries the compare l[kc] (cmp index, #N) lets the table load l[kl] read, or nullopt when no branch on its
// flags guards the load. A compare bounds the index only through the branch that acts on it. Between the compare and
// the load, the first branch on an unsigned condition decides, with the flags still the compare's:
//   b.hi out -> N + 1 entries, b.hs / b.cs out -> N: the dispatch is the fall-through, and the branch leaves it;
//   b.ls in -> N + 1, b.lo / b.cc in -> N: the branch enters the dispatch and the instruction before its target is a
//   b, br or ret, so the path that failed the check does not fall into it.
// Any other branch on the flags is no bounds check, and so is a compare with none (guarded_count).
std::optional<Addr> GuardedCount(const InsnList& l, Index kc, Index kl) {
    const Addr n = ImmAfterHash(l[kc]);
    const Insn& ld = l[kl];
    for (Index q = kc + 1; q < kl; ++q) {
        const Insn& p = l[q];
        if (p.IsAny("b.hi", "b.hs", "b.cs", "b.ls", "b.lo", "b.cc")) {
            const Addr t = p.operands[0].imm;
            const bool inside = p.address < t && t <= ld.address;
            if (p.IsAny("b.hi", "b.hs", "b.cs")) {
                if (inside) return std::nullopt;
                return n + (p.Is("b.hi") ? 1 : 0);
            }
            const Insn* before = nullptr;
            for (Index x = q + 1; x <= kl && before == nullptr; ++x) {
                if (l[x].address == t - 4) before = &l[x];
            }
            if (inside && before != nullptr && before->IsAny("b", "br", "ret")) return n + (p.Is("b.ls") ? 1 : 0);
            return std::nullopt;
        }
        if (p.IsAny("cmp", "cmn", "tst", "ccmp", "ccmn", "fcmp", "fcmpe", "fccmp", "fccmpe", "adds", "subs", "ands",
                    "bics", "adcs", "sbcs", "negs", "ngcs", "msr", "bl", "blr") ||
            IsBCond(p) || p.IsAny("b", "br", "ret")) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// The index copied from the checked register before the check, far back: accepted when the cmp and its b.hi come right
// before the load l[ld] and the mov is the last writer of the index and of the checked register before them. The
// writers are followed to the function's start, stopping at calls that can clobber either register or at gaps in the
// decoded code. -> the cmp's index, or -1 (check_before_copy)
Index CheckBeforeCopy(const InsnList& l, Index c0, Index ld, std::uint32_t idx) {
    if (ld - c0 < 2 || !l[ld - 1].Is("b.hi") || !l[ld - 2].Is("cmp")) return -1;
    std::string_view src_name;
    if (!Scan(l[ld - 2].op_str, "%w, #%i", {&src_name})) return -1;
    const std::uint32_t src = BitOf(src_name);
    const std::uint32_t both = idx | src;
    for (Index q = ld - 3; q >= 0; --q) {
        const Insn& p = l[q];
        if (p.address + 4 != l[q + 1].address) return -1;
        if (p.IsAny("bl", "blr") && (both & 0x7FFFF) != 0) return -1;
        std::string_view to, from;
        if (p.Is("mov") && Scan(p.op_str, "%w, %w", {&to, &from}) && (BitOf(to) & idx) != 0 && BitOf(from) == src) {
            return ld - 2;
        }
        if (!IsCompare(p) && (WrittenMask(p) & both) != 0) return -1;
    }
    return -1;
}

// The address the table base register rb holds at l[k]: its writer chain before k must be add rb,rb,#lo after adrp rb.
// It is searched 256 instructions back, or with far to the function's start, through no call when rb is caller-saved
// (Capstone lists only x30 as a call's write) (table_base).
std::optional<Addr> TableBase(const InsnList& l, Index k, const std::string& rb, bool far) {
    std::optional<Addr> lo;
    const int rbn = NumberOf(rb);
    const std::string add = absl::StrCat(rb, ", ", rb, ", #%i");
    for (Index i = k - 1; i >= (far ? 0 : std::max<Index>(0, k - 256)); --i) {
        const Insn& p = l[i];
        if (far && p.IsAny("bl", "blr") && rbn <= 18) return std::nullopt;
        if (!WritesReg(p, rbn)) continue;
        if (!lo && p.Is("add") && Scan(p.op_str, add)) {
            lo = ImmAfterHash(p);
            continue;
        }
        if (lo && p.Is("adrp") && p.op_str.starts_with(rb + ",")) return p.operands[1].imm + *lo;
        return std::nullopt;
    }
    return std::nullopt;
}

// The address rb holds at l[k] when it was built by adrp + add and then only copied between registers (mov xD, xS): the
// writer chain is walked back to the function's start, switching to the source register at each copy, through no call
// while the register followed is caller-saved; nullopt for any other writer (copied_base).
std::optional<Addr> CopiedBase(const InsnList& l, Index k, const std::string& rb) {
    std::optional<Addr> lo;
    std::string r(Canon(rb));
    for (Index i = k - 1; i >= 0; --i) {
        const Insn& p = l[i];
        if (p.IsAny("bl", "blr") && RegNum(r).value_or(0) <= 18) return std::nullopt;
        // A write to wN zeroes the upper half of xN too: it is a clobber, never a copy of the 64-bit base
        bool writes = false;
        for (const std::uint16_t reg : p.regs_write) {
            std::string name(Canon(p.Name(reg)));
            if (const std::size_t w = name.find('w'); w != std::string::npos) name[w] = 'x';
            writes = writes || name == r;
        }
        if (!writes) continue;
        std::vector<std::string_view> ops;
        for (std::string_view rest = p.op_str;;) {
            const std::size_t comma = rest.find(", ");
            ops.push_back(Canon(rest.substr(0, comma)));
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 2);
        }
        if (!lo && p.Is("mov") && ops.size() == 2 && ops[0] == r && Scan(ops[1], "%x")) {
            r = std::string(ops[1]);
            continue;
        }
        if (!lo && p.Is("add") && ops.size() == 3 && ops[0] == r && ops[1] == r && Scan(ops[2], "#%i")) {
            lo = ParseInt(ops[2].substr(1));
            continue;
        }
        if (lo && p.Is("adrp") && ops[0] == r) return p.operands[1].imm + *lo;
        return std::nullopt;
    }
    return std::nullopt;
}

// Every address an adrp rb + add rb,rb,#lo pair in the function builds, sorted (table_cands)
std::vector<Addr> TableCands(const InsnList& l, const std::string& rb) {
    std::vector<Addr> cands;
    const int rbn = NumberOf(rb);
    const std::string add = absl::StrCat(rb, ", ", rb, ", #%i");
    for (std::size_t k = 0; k < l.size(); ++k) {
        const Insn& p = l[k];
        if (!p.Is("adrp") || !p.op_str.starts_with(rb + ",")) continue;
        for (std::size_t x = k + 1; x < std::min(k + 9, l.size()); ++x) {
            if (!WritesReg(l[x], rbn)) continue;
            if (l[x].Is("add") && Scan(l[x].op_str, add)) cands.push_back(p.operands[1].imm + ImmAfterHash(l[x]));
            break;
        }
    }
    std::sort(cands.begin(), cands.end());
    cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
    return cands;
}

// The registers p writes by Capstone's list, as names
bool WritesNamed(const Insn& p, std::initializer_list<std::string_view> names) {
    for (const std::uint16_t reg : p.regs_write) {
        const std::string_view name = p.Name(reg);
        if (std::find(names.begin(), names.end(), name) != names.end()) return true;
    }
    return false;
}

// ---- br x18 as an indirect tail call ----

// The last write of x18 before the br is a plain pointer load and the br ends the function (tail_call_at_end)
bool TailCallAtEnd(const InsnList& l, std::size_t j) {
    if (j + 1 != l.size()) return false;
    for (Index i = static_cast<Index>(j) - 1; i >= 0; --i) {
        const Insn& p = l[i];
        if (!WritesNamed(p, {"x18"})) continue;
        return p.Is("ldr") && (Scan(p.op_str, "x18, [%x]") || Scan(p.op_str, "x18, [sp]") ||
                               Scan(p.op_str, "x18, [%x, #%i]") || Scan(p.op_str, "x18, [sp, #%i]"));
    }
    return false;
}

// x18, (x\d+, )?\[(x\d+|sp)(, #imm)?\]: a plain pointer load into x18, alone or as a pair's first register
bool PointerLoad18(std::string_view op) {
    for (const char* first : {"x18, ", "x18, %x, "}) {
        for (const char* base : {"[%x", "[sp"}) {
            for (const char* disp : {"]", ", #%i]"}) {
                if (Scan(op, absl::StrCat(first, base, disp))) return true;
            }
        }
    }
    return false;
}

// An unframed, straight-line pointer thunk that ends in br x18 and zero trap padding (tail_call_padded)
bool TailCallPadded(const InsnList& l, std::size_t j) {
    if (j + 1 >= l.size()) return false;
    for (std::size_t i = j + 1; i < l.size(); ++i) {
        if (!l[i].Is("udf") || l[i].op_str != "#0") return false;
    }
    if (!Contiguous(l, 0, static_cast<Index>(l.size()) - 1)) return false;
    for (std::size_t i = 0; i < j; ++i) {
        if (!l[i].IsAny("mov", "ldr", "ldrh", "ldp", "add", "str", "stp")) return false;
        if (WritesNamed(l[i], {"sp", "wsp", "fp", "lr", "x29", "w29", "x30", "w30"})) return false;
    }
    for (Index i = static_cast<Index>(j) - 1; i >= 0; --i) {
        if (!WritesNamed(l[i], {"x18", "w18"})) continue;
        return l[i].IsAny("ldr", "ldp") && PointerLoad18(l[i].op_str);
    }
    return false;
}

// After terminal or padded unframed tails, a br x18 with other code after it is a tail call only when the frame is
// provably gone: a jump inside a function needs that function's frame. A wrong accept is silent, where a rejection
// stops at "jump table <addr>", so anything not proven here is rejected. Two forms (Unity IL2CPP): a 16-byte thunk,
// stp x29,x30,[sp,#-0x10]!; mov x29,sp, only guards and argument moves, its error blocks after the br; or an ordinary
// function, sub sp,sp,#N first and add sp,sp,#N right before the br, the pair saved to and restored from one slot,
// whose body may branch and call and whose other paths may follow the br. Either way nothing writes sp, x29 or x30 on
// the way, no stack store touches the pair, and from the restore to the br every path runs straight (tail_call).
bool TailCall(const InsnList& l, std::size_t ju) {
    const auto j = static_cast<Index>(ju);
    const auto size = static_cast<Index>(l.size());
    if (j + 1 == size) return TailCallAtEnd(l, ju);
    if (TailCallPadded(l, ju)) return true;
    if (j < 4 || !Contiguous(l, 0, size - 1)) return false;
    const auto cond = [](const Insn& p) { return IsCbTb(p) || IsBCond(p); };
    // The last write of x18 is a plain pointer load
    Index load = -1;
    for (Index k = j - 1; k >= 0 && load < 0; --k) {
        if (WritesNamed(l[k], {"x18", "w18"})) load = k;
    }
    if (load < 0 || !l[load].IsAny("ldr", "ldp") || !PointerLoad18(l[load].op_str)) return false;
    // The frame. disp is the saved pair's offset from sp
    const bool thunk = l[0].Is("stp") && l[0].op_str == "x29, x30, [sp, #-0x10]!";
    Index restore = -1;
    Index start = 0;
    Addr disp = 0;
    Addr frame = 0;
    bool saved = false;
    bool fp_set = false;
    if (thunk) {
        if (!l[1].Is("mov") || l[1].op_str != "x29, sp") return false;
        if (!l[j - 1].Is("ldp") || l[j - 1].op_str != "x29, x30, [sp], #0x10") return false;
        restore = j - 1;
        start = 2;
        saved = fp_set = true;
    } else {
        std::string_view sub, add;
        if (!l[0].Is("sub") || !Scan(l[0].op_str, "sp, sp, #%i", {&sub})) return false;
        if (!l[j - 1].Is("add") || !Scan(l[j - 1].op_str, "sp, sp, #%i", {&add})) return false;
        frame = ParseInt(sub);
        if (frame != ParseInt(add)) return false;
        for (Index k = j - 2; k > 0 && restore < 0; --k) {
            if (l[k].Is("ldp") && (l[k].op_str == "x29, x30, [sp]" || Scan(l[k].op_str, "x29, x30, [sp, #%i]"))) {
                restore = k;
            }
        }
        if (restore < 0) return false;
        disp = l[restore].operands[l[restore].op_count - 1].mem.disp;
        if (disp < 0 || disp + 16 > frame) return false;
        start = 1;
    }
    const Index tail = std::min(load, restore);  // no call or jump from here on
    const Addr after_br = l[j].address;
    Addr setup_end = l[start].address;  // no branch re-enters the frame's setup: raised as the setup is walked
    std::vector<Addr> targets;
    bool guarded = false;
    for (Index k = start; k < j - 1; ++k) {  // the body and the epilogue; j - 1 is the frame's last restore or add sp
        const Insn& p = l[k];
        if (k == restore) continue;
        if (cond(p) || p.Is("b")) {
            const Addr t = p.Target();
            if (!saved || k >= restore || (p.Is("b") && k >= tail)) return false;
            if (t > after_br) {
                guarded = true;
            } else if (thunk) {
                return false;
            }
            targets.push_back(t);
            continue;
        }
        if (p.IsAny("bl", "blr")) {
            // A saved x30, and x18 not live across the call
            if (thunk || !saved || k >= tail) return false;
            continue;
        }
        if (p.IsAny("br", "ret", "udf", "brk", "hlt", "svc", "hvc", "smc", "msr", "sys", "dc", "ic")) return false;
        if (WritesNamed(p, {"sp", "wsp", "lr", "x30", "w30"})) return false;
        if (WritesNamed(p, {"fp", "x29", "w29"})) {
            const bool sets_fp = p.Is("mov")
                                     ? (p.op_str == "x29, sp" && disp == 0)
                                     : (p.Is("add") && p.op_str.starts_with("x29, sp, #") && p.operands[2].imm == disp);
            if (k > restore || fp_set || !saved || !sets_fp) return false;
            fp_set = true;
            setup_end = l[k + 1].address;
            continue;
        }
        if (!saved && p.Is("stp") && p.op_str == l[restore].op_str) {
            saved = true;
            setup_end = l[k + 1].address;
            continue;
        }
        const std::string_view m = p.mnemonic;
        if (!absl::StrContains(p.op_str, '[') || m.starts_with("ldr") || m.starts_with("ldur") ||
            m.starts_with("ldp") || m.starts_with("ldar") || m.starts_with("prfm")) {
            continue;
        }
        // Capstone does not list memory writes, so a stack store's bytes are checked against the saved pair. A
        // register offset is unproven. A store through another base can alias it only through a pointer to the
        // frame's own pair, which no compiled code holds; only the thunk rejects them.
        if (k > restore || !p.IsAny("str", "strb", "strh", "stur", "sturb", "sturh", "stp") || p.writeback)
            return false;
        const arm64_op_mem& mem = p.operands[p.op_count - 1].mem;
        const std::string_view base = p.Name(mem.base);
        if (base != "sp" && base != "fp" && base != "x29") {
            if (thunk) return false;
            continue;
        }
        if (mem.index != 0 || (base != "sp" && !fp_set)) return false;
        const Addr at = mem.disp + (base != "sp" ? disp : 0);
        Addr bytes = m.back() == 'b' ? 1 : m.back() == 'h' ? 2 : 0;
        if (bytes == 0) {
            const std::string_view reg = p.OpReg(0);
            const char c = reg.empty() ? ' ' : reg[0];
            bytes = c == 'x'   ? 8
                    : c == 'w' ? 4
                    : c == 'q' ? 16
                    : c == 'd' ? 8
                    : c == 's' ? 4
                    : c == 'h' ? 2
                    : c == 'b' ? 1
                               : 8;
        }
        if (p.Is("stp")) bytes *= 2;
        if (at < 0 || (at < disp + 16 && at + bytes > disp)) return false;
    }
    if (!saved) return false;
    // What follows the br never comes back to a place whose stack it cannot vouch for. A thunk's is error blocks
    // entered by a guard, leaving through a call or a trap; a function's may be any of its other paths.
    bool leaves = false;
    for (Index k = j + 1; k < size; ++k) {
        const Insn& p = l[k];
        if (p.Is("bl")) {
            if (l[0].address <= p.Target() && p.Target() <= l[size - 1].address) return false;
            leaves = true;
        } else if ((cond(p) || p.Is("b")) && p.Target() <= after_br) {
            return false;
        }
    }
    if (thunk) {
        if (!guarded || !leaves || !l[size - 1].IsAny("bl", "udf", "b")) return false;
        for (Index k = j + 1; k < size; ++k) {
            if (l[k].IsAny("ret", "br", "blr")) return false;
        }
    }
    // Nothing re-enters the frame's setup, or lands past the restore: the frame's end is what the proof rests on
    for (const Addr t : targets) {
        if (t < setup_end || (l[restore].address < t && t <= after_br)) return false;
    }
    return true;
}

}  // namespace

std::vector<InsnList> Rewriter::FindWindows(const InsnList& insl) {
    std::vector<InsnList> out;
    for (std::size_t i = 0; i < insl.size(); ++i) {
        if (!IsExclusiveLoad(insl[i])) continue;
        std::size_t j = i;
        while (j < insl.size() && j - i < 16 && !IsExclusiveStore(insl[j])) ++j;
        if (j >= insl.size() || j - i >= 16) continue;
        // Another exclusive load before the store: this one has no store of its own (a retry block); spanning both
        // would merge two sequences
        bool second = false;
        for (std::size_t x = i + 1; x < j; ++x) second = second || IsExclusiveLoad(insl[x]);
        if (second) continue;
        if (j + 1 < insl.size() && insl[j + 1].IsAny("cbz", "cbnz")) ++j;
        bool uses18 = false;
        for (std::size_t x = i; x <= j; ++x) uses18 = uses18 || Has18(insl[x].op_str);
        if (uses18) out.emplace_back(insl.begin() + static_cast<Index>(i), insl.begin() + static_cast<Index>(j) + 1);
    }
    return out;
}

std::optional<std::pair<InsnList, InsnList>> Rewriter::RetryBlock(const InsnList& insl, std::size_t k,
                                                                  const InsnList& wseg) {
    // The branch insl[k] lands in the middle of window wseg. Accepted only when it ends an out-of-line retry block
    // that re-enters the window at its store-exclusive: an exclusive load, then instructions that do not branch, then
    // this branch. -> (the retry block, the window from that store on)
    const Addr t = insl[k].Target();
    InsnList tail;
    for (const Insn& y : wseg) {
        if (y.address >= t) tail.push_back(y);
    }
    if (tail.empty() || !IsExclusiveStore(tail[0])) return std::nullopt;
    for (Index p = static_cast<Index>(k) - 1; p > std::max<Index>(-1, static_cast<Index>(k) - 8); --p) {
        const Insn& q = insl[p];
        if (IsExclusiveLoad(q)) {
            return std::pair(InsnList(insl.begin() + p, insl.begin() + static_cast<Index>(k) + 1), std::move(tail));
        }
        if (q.IsAny("b", "bl", "br", "blr", "ret") || IsCbTb(q) || IsBCond(q) || IsExclusiveStore(q))
            return std::nullopt;
    }
    return std::nullopt;
}

// n signed entries of the table at tb, each added to base; a RangeError when the table lies outside the image
std::vector<Addr> Rewriter::ReadTable(Addr tb, Addr n, Addr base) const {
    std::vector<Addr> out;
    out.reserve(static_cast<std::size_t>(std::max<Addr>(n, 0)));
    for (Addr i = 0; i < n; ++i) out.push_back(base + I32(img_, tb + 4 * i));
    return out;
}

// The br x18 at insl[j] is preceded by the offset-table form: cmp w18,#N; b.hi; ldrsw x18,[xB,x18,lsl #2];
// add x18,x18,xB. The index may be another register, the bounds check may be on a register copied into it or on a
// masked copy, and the entry may be loaded into another register with only the add writing x18. The table base xB comes
// from the nearest add xB,xB,#lo after adrp xB; with far_base a base that nothing writes in 256 instructions is
// followed to the function's start, and every target must then be an instruction of the function. -> all targets, or
// none when the form is not there (jump_offsets)
std::vector<Addr> Rewriter::JumpOffsets(const InsnList& insl, std::size_t ju, bool copy_first, bool far_base,
                                        bool forward_base) const {
    const auto j = static_cast<Index>(ju);
    const Index c0 = std::max<Index>(0, j - 16);
    Index add = -1;
    std::string_view t_name, rb_name;
    for (Index i = j - 1; i >= c0 && add < 0; --i) {
        if (insl[i].Is("add") && Scan(insl[i].op_str, "x18, %x, %x", {&t_name, &rb_name})) add = i;
    }
    if (add < 0) return {};
    const std::string t(t_name);    // the loaded entry
    const std::string rb(rb_name);  // the table base
    const int rbn = NumberOf(rb);
    const std::string load = absl::StrCat(t, ", [", rb, ", x%n, lsl #2]");
    Index ld = -1;
    std::string_view ix;
    for (Index i = j - 1; i >= c0 && ld < 0; --i) {
        if (insl[i].Is("ldrsw") && Scan(insl[i].op_str, load, {&ix})) ld = i;
    }
    if (ld < 0) return {};
    std::uint32_t idx = Bit(static_cast<int>(ParseInt(ix)));
    struct Masked {
        std::uint32_t source;
        Addr mask;
        Index at;
    };
    std::optional<Masked> masked;  // when the index is "source & mask"
    Index cmp = -1;
    Addr n = 0;
    std::uint32_t copies = 0;
    bool changed = false;
    for (Index i = ld - 1; i >= c0; --i) {  // walk back from the load: follow copies into the index, stop at the check
        const Insn& p = insl[i];
        std::string_view a, b, imm;
        if (p.Is("mov") && Scan(p.op_str, "%w, %w", {&a, &b}) && (BitOf(a) & idx) != 0) {
            idx = (idx & ~BitOf(a)) | BitOf(b);  // a copy replaces the index: a check of its old value bounds nothing
            copies |= BitOf(a);
            continue;
        }
        if (p.Is("and") && Scan(p.op_str, "%r, %r, #%i", {&a, &b, &imm}) && (BitOf(a) & idx) != 0 && !masked) {
            masked = Masked{BitOf(b), ParseInt(imm), i};
            continue;
        }
        // A 64-bit check of the index bounds it as well; taken only in the last attempt (far_base)
        if (!(p.Is("cmp") && Scan(p.op_str, far_base ? "%r, #%i" : "%w, #%i", {&a}))) {
            // Capstone lists only x30 as a call's write, so a call counts for the caller-saved registers
            if ((!IsCompare(p) && (WrittenMask(p) & idx) != 0) || (p.IsAny("bl", "blr") && (idx & 0x7FFFF) != 0)) {
                changed = true;
            }
            continue;
        }
        // Only a compare a branch acts on bounds the index: one that guards nothing only sets the flags
        if ((BitOf(a) & idx) != 0) {
            const std::optional<Addr> g = GuardedCount(insl, i, ld);
            if (!g) continue;
            cmp = i;
            n = *g;
            break;
        }
        if (masked) {  // the checked register must be "the same source & the same mask", the source unchanged between
            for (Index q = i - 1; q >= c0; --q) {
                if ((WrittenMask(insl[q]) & BitOf(a)) == 0) continue;
                std::string_view ma, mb, mimm;
                if (insl[q].Is("and") && Scan(insl[q].op_str, "%r, %r, #%i", {&ma, &mb, &mimm}) &&
                    BitOf(mb) == masked->source && ParseInt(mimm) == masked->mask) {
                    bool source_written = false;
                    for (Index z = q + 1; z < masked->at; ++z) {
                        source_written = source_written || (WrittenMask(insl[z]) & masked->source) != 0;
                    }
                    if (!source_written) {
                        if (const std::optional<Addr> g = GuardedCount(insl, i, ld)) {
                            cmp = i;
                            n = *g;
                        }
                    }
                }
                break;
            }
            if (cmp >= 0) break;
        }
    }
    if (cmp >= 0 && changed) return {};  // the index changes after the check
    if (cmp < 0 && copy_first) {
        cmp = CheckBeforeCopy(insl, c0, ld, idx | copies);  // cmp; b.hi right before the load
        if (cmp >= 0) n = ImmAfterHash(insl[cmp]) + 1;
    }
    if (cmp < 0) return {};
    const Addr fa = insl.front().address;
    const Addr fb = insl.back().address + 4;
    const auto in_function = [&](const std::vector<Addr>& targets) {
        return std::all_of(targets.begin(), targets.end(), [&](Addr x) { return fa <= x && x < fb && x % 4 == 0; });
    };
    const auto size = static_cast<Addr>(img_.size());
    std::optional<Addr> lo, page;
    bool broke = false;
    const std::string base_add = absl::StrCat(rb, ", ", rb, ", #%i");
    for (Index i = ld - 1; i >= std::max<Index>(0, ld - 256); --i) {  // the writer chain of the table base
        const Insn& p = insl[i];
        if (!WritesReg(p, rbn)) continue;
        if (!lo && NullBase(p, n)) continue;  // page zero as the base: it reaches no load of a working game
        if (!lo && p.Is("add") && Scan(p.op_str, base_add)) {
            lo = ImmAfterHash(p);
            continue;
        }
        if (lo && p.Is("adrp") && p.op_str.starts_with(rb + ",")) page = p.operands[1].imm;
        broke = true;
        break;
    }
    if (!broke && !lo && far_base) {  // nothing writes the base in the window
        std::optional<Addr> tb = TableBase(insl, ld, rb, true);
        if (!tb) tb = CopiedBase(insl, ld, rb);  // built further back and copied on its way
        if (!tb && forward_base) tb = ForwardTableBase(insl, static_cast<std::size_t>(ld), rb, n);
        if (!tb || *tb + 4 * n > size) return {};
        std::vector<Addr> targets = ReadTable(*tb, n, *tb);
        return in_function(targets) ? targets : std::vector<Addr>();
    }
    if (!page && far_base) {
        // The base was built further back and passed through other registers on its way: the copies are followed back
        // to the function's start, and every target must be an instruction of this function
        const std::optional<Addr> tb = CopiedBase(insl, ld, rb);
        if (!tb || *tb + 4 * n > size) return {};
        std::vector<Addr> targets = ReadTable(*tb, n, *tb);
        return in_function(targets) ? targets : std::vector<Addr>();
    }
    if (!page) return {};
    const Addr tb = *page + *lo;
    return ReadTable(tb, n, tb);
}

// cmp wS,#N; b.hi exit; and xI,xS,#0xffffffff; ldrsw xE,[xB,xI,lsl #2]; add x18,xE,xB; br x18: the checked value is
// zero-extended rather than a masked copy checked. Straight-line data flow from the guard to the br, no entry past the
// check, and decoded targets outside that run. Tried after every older recognizer (jump_zero_extended).
std::vector<Addr> Rewriter::JumpZeroExtended(const InsnList& insl, std::size_t ju) const {
    const auto j = static_cast<Index>(ju);
    const Index c0 = std::max<Index>(0, j - 16);
    Index ka = -1;
    std::string_view ent_name, base_name;
    for (Index i = j - 1; i >= c0 && ka < 0; --i) {
        if (insl[i].Is("add") && Scan(insl[i].op_str, "x18, %x, %x", {&ent_name, &base_name})) ka = i;
    }
    if (ka < 0) return {};
    const std::string ent(ent_name), base(base_name);
    Index kl = -1;
    std::string_view ix;
    const std::string load = absl::StrCat(ent, ", [", base, ", x%n, lsl #2]");
    for (Index i = ka - 1; i >= c0 && kl < 0; --i) {
        if (insl[i].Is("ldrsw") && Scan(insl[i].op_str, load, {&ix})) kl = i;
    }
    if (kl < 0) return {};
    const std::string idx = absl::StrCat("x", ix);
    if (base == ent || base == idx) return {};
    Index km = -1;
    std::string_view src_name;
    const std::string mask = absl::StrCat(idx, ", %x, #0xffffffff");
    for (Index i = kl - 1; i >= c0 && km < 0; --i) {
        if (insl[i].Is("and") && Scan(insl[i].op_str, mask, {&src_name})) km = i;
    }
    if (km < 0) return {};
    const std::string src(src_name);
    Index kc = -1;
    const std::string check = absl::StrCat("w", src.substr(1), ", #%i");
    for (Index i = km - 1; i >= c0 && kc < 0; --i) {
        if (insl[i].Is("cmp") && Scan(insl[i].op_str, check)) kc = i;
    }
    if (kc < 0) return {};
    const Insn& branch = insl[kc + 1];
    if (!branch.Is("b.hi")) return {};
    const Addr head = insl[kc].address;
    const Addr end = insl[j].address;
    if (!Contiguous(insl, kc, j)) return {};
    if (head <= branch.operands[0].imm && branch.operands[0].imm <= end) return {};
    // Calls and system calls can overwrite registers without appearing in Capstone's writes: no control flow or trap
    // after the guard
    for (Index i = kc + 2; i < j; ++i) {
        const Insn& p = insl[i];
        if (p.IsAny("b", "bl", "br", "blr", "ret", "cbz", "cbnz", "tbz", "tbnz", "svc", "hvc", "smc", "brk", "hlt",
                    "eret", "drps") ||
            IsBCond(p)) {
            return {};
        }
    }
    // Capstone lists a compare's first operand as written; only its flags change. A register without a name counts.
    const auto writes = [](const Insn& p, const std::string& reg) {
        if (IsCompare(p)) return false;
        const std::string w = "w" + reg.substr(1);
        const std::string_view alias = reg == "x29" ? "fp" : reg == "x30" ? "lr" : "";
        for (const std::uint16_t r : p.regs_write) {
            const std::string_view name = p.Name(r);
            if (name.empty() || name == reg || name == w || (!alias.empty() && name == alias)) return true;
        }
        return false;
    };
    const auto any_writes = [&](Index from, Index to, const std::string& reg) {
        for (Index i = from; i < to; ++i) {
            if (writes(insl[i], reg)) return true;
        }
        return false;
    };
    if (any_writes(kc + 2, km, src) || any_writes(km + 1, kl, idx) || any_writes(kl + 1, ka, ent) ||
        any_writes(kl + 1, ka, base) || any_writes(ka + 1, j, "x18")) {
        return {};
    }
    for (const Insn& p : insl) {
        if ((p.IsAny("b", "bl") || IsCbTb(p) || IsBCond(p)) && head < p.Target() && p.Target() <= end) return {};
    }
    for (const auto& [at, targets] : tabjumps_) {
        for (const Addr t : targets) {
            if (head < t && t <= end) return {};
        }
    }
    for (const auto& [at, table] : jumps_) {
        if (table.first <= end && table.first + 4 * table.second > head + 4) return {};
    }
    const std::optional<Addr> tb = CopiedBase(insl, kl, base);
    const Addr n = ImmAfterHash(insl[kc]) + 1;
    if (!tb || *tb < 0 || *tb + 4 * n > static_cast<Addr>(img_.size())) return {};
    std::vector<Addr> targets = ReadTable(*tb, n, *tb);
    absl::flat_hash_set<Addr> addresses;
    for (const Insn& p : insl) addresses.insert(p.address);
    for (const Addr t : targets) {
        if (!addresses.contains(t) || (head < t && t <= end)) return {};
    }
    return targets;
}

// A loop's base initialized after its dispatch in address order (the block then branches backward). Used only after
// every older recognizer failed: exactly two writers of the full base in the function, an adjacent adrp + add after
// the load, no call that can clobber it, and no known entry into the add that bypasses the adrp (forward_table_base).
std::optional<Addr> Rewriter::ForwardTableBase(const InsnList& insl, std::size_t ku, const std::string& rb,
                                               Addr n) const {
    const auto k = static_cast<Index>(ku);
    const auto size = static_cast<Index>(insl.size());
    if (k < 2 || k + 2 >= size) return std::nullopt;
    const Insn& guard = insl[k - 2];
    const Insn& branch = insl[k - 1];
    const Insn& load = insl[k];
    const Insn& add = insl[k + 1];
    const Insn& jump = insl[k + 2];
    std::string_view ent, ix;
    if (!load.Is("ldrsw") || !Scan(load.op_str, absl::StrCat("%x, [", rb, ", x%n, lsl #2]"), {&ent, &ix}) ||
        !guard.Is("cmp")) {
        return std::nullopt;
    }
    std::string_view bound;
    if (!Scan(guard.op_str, absl::StrCat("w", ix, ", #%i"), {&bound}) || ParseInt(bound) != n - 1) return std::nullopt;
    if (!branch.Is("b.hi") || !add.Is("add") || add.op_str != absl::StrCat("x18, ", ent, ", ", rb) || !jump.Is("br") ||
        jump.op_str != "x18") {
        return std::nullopt;
    }
    if (!Contiguous(insl, k - 2, k + 2)) return std::nullopt;
    std::set<Addr> guarded;
    for (Index i = k - 1; i < k + 3; ++i) guarded.insert(insl[i].address);
    const int rbn = NumberOf(rb);
    if (rbn <= 18 && std::any_of(insl.begin(), insl.end(), [](const Insn& p) { return p.IsAny("bl", "blr"); })) {
        return std::nullopt;
    }
    const std::string w = "w" + rb.substr(1);
    const std::string_view alias = rb == "x30" ? "lr" : rb == "x29" ? "fp" : "";
    std::vector<Index> writers;
    for (Index i = 0; i < size; ++i) {
        if (IsCompare(insl[i])) continue;
        bool hit = false;
        for (const std::uint16_t r : insl[i].regs_write) {
            const std::string_view name = insl[i].Name(r);
            hit = hit || name == rb || name == w || (!alias.empty() && name == alias);
        }
        if (hit) writers.push_back(i);
    }
    if (writers.size() != 2) return std::nullopt;
    const Insn& p = insl[writers[0]];
    const Insn& q = insl[writers[1]];
    if (writers[0] <= k || q.address != p.address + 4 || !p.Is("adrp") || !p.op_str.starts_with(rb + ",")) {
        return std::nullopt;
    }
    std::string_view lo;
    if (!q.Is("add") || !Scan(q.op_str, absl::StrCat(rb, ", ", rb, ", #%i"), {&lo})) return std::nullopt;
    guarded.insert(q.address);
    for (const Insn& x : insl) {
        if ((x.IsAny("b", "bl") || IsCbTb(x) || IsBCond(x)) && guarded.contains(x.Target())) return std::nullopt;
    }
    for (const auto& [at, targets] : tabjumps_) {
        for (const Addr t : targets) {
            if (guarded.contains(t)) return std::nullopt;
        }
    }
    for (const auto& [at, table] : jumps_) {
        for (const Addr x : guarded) {
            if (table.first <= x && x < table.first + 4 * table.second) return std::nullopt;
        }
    }
    const Addr tb = p.operands[1].imm + ParseInt(lo);
    if (!(0 <= tb && tb <= static_cast<Addr>(img_.size()) - 4 * n)) return std::nullopt;
    absl::flat_hash_set<Addr> addresses;
    for (const Insn& x : insl) addresses.insert(x.address);
    for (Addr z = 0; z < n; ++z) {
        const Addr target = tb + I32(img_, tb + 4 * z);
        if (guarded.contains(target) || !addresses.contains(target)) return std::nullopt;
    }
    return tb;
}

// A zero-guarded complemented index: tst wS,#M; b.eq exit; mvn wS,wS; and xI,xS,#M; ldrsw xE,[xB,xI,lsl #2];
// add x18,xE,xB; br x18. M entries for a contiguous low-bit mask (the tst excludes zero and the complement maps 1..M to
// M-1..0), only after the older recognizers fail (jump_masked_not).
std::vector<Addr> Rewriter::JumpMaskedNot(const InsnList& insl, std::size_t ju) const {
    const auto j = static_cast<Index>(ju);
    const Index c0 = std::max<Index>(0, j - 16);
    Index ka = -1;
    std::string_view ent_name, base_name;
    for (Index i = j - 1; i >= c0 && ka < 0; --i) {
        if (insl[i].Is("add") && Scan(insl[i].op_str, "x18, %x, %x", {&ent_name, &base_name})) ka = i;
    }
    if (ka < 0) return {};
    const std::string ent(ent_name), base(base_name);
    Index kl = -1;
    std::string_view ix;
    const std::string load = absl::StrCat(ent, ", [", base, ", x%n, lsl #2]");
    for (Index i = ka - 1; i >= c0 && kl < 0; --i) {
        if (insl[i].Is("ldrsw") && Scan(insl[i].op_str, load, {&ix})) kl = i;
    }
    if (kl < 0) return {};
    const std::string idx = absl::StrCat("x", ix);
    if (base == ent || base == idx) return {};
    Index km = -1;
    std::string_view src_name, imm;
    const std::string mask = absl::StrCat(idx, ", %x, #%i");
    for (Index i = kl - 1; i >= c0 && km < 0; --i) {
        if (insl[i].Is("and") && Scan(insl[i].op_str, mask, {&src_name, &imm})) km = i;
    }
    if (km < 0 || km - c0 < 3) return {};
    const std::string wsrc = absl::StrCat("w", src_name.substr(1));
    const Addr n = ParseInt(imm);
    if (n == 0 || (n & (n + 1)) != 0 || n > 0xffffffff) return {};
    const Insn& test = insl[km - 3];
    const Insn& branch = insl[km - 2];
    const Insn& invert = insl[km - 1];
    if (!test.Is("tst") || test.op_str != absl::StrCat(wsrc, ", #", imm) || !branch.Is("b.eq") || !invert.Is("mvn") ||
        invert.op_str != absl::StrCat(wsrc, ", ", wsrc)) {
        return {};
    }
    if (!Contiguous(insl, km - 3, j)) return {};
    const Addr head = test.address;
    const Addr end = insl[j].address;
    if (head <= branch.operands[0].imm && branch.operands[0].imm <= end) return {};
    const auto writes = [](const Insn& p, const std::string& reg) {
        const int n_reg = NumberOf(reg);
        if (p.IsAny("bl", "blr") && n_reg <= 18) return true;
        return WritesReg(p, n_reg);
    };
    const auto any_writes = [&](Index from, Index to, const std::string& reg) {
        for (Index i = from; i < to; ++i) {
            if (writes(insl[i], reg)) return true;
        }
        return false;
    };
    if (any_writes(km + 1, kl, idx) || any_writes(kl + 1, ka, ent) || any_writes(kl + 1, ka + 1, base) ||
        any_writes(ka + 1, j, "x18")) {
        return {};
    }
    for (Index i = km + 1; i < j; ++i) {
        if (insl[i].IsAny("b", "bl", "br", "blr", "ret") || IsCbTb(insl[i]) || IsBCond(insl[i])) return {};
    }
    for (const Insn& p : insl) {
        if ((p.IsAny("b", "bl") || IsCbTb(p) || IsBCond(p)) && head < p.Target() && p.Target() <= end) return {};
    }
    for (const auto& [at, targets] : tabjumps_) {
        for (const Addr t : targets) {
            if (head < t && t <= end) return {};
        }
    }
    for (const auto& [at, table] : jumps_) {
        if (table.first <= end && table.first + 4 * table.second > head + 4) return {};
    }
    const std::optional<Addr> tb = CopiedBase(insl, kl, base);
    if (!tb || *tb < 0 || *tb + 4 * n > static_cast<Addr>(img_.size())) return {};
    std::vector<Addr> targets = ReadTable(*tb, n, *tb);
    absl::flat_hash_set<Addr> addresses;
    for (const Insn& p : insl) addresses.insert(p.address);
    for (const Addr t : targets) {
        if (!addresses.contains(t) || (head < t && t <= end)) return {};
    }
    return targets;
}

// Entries relative to an adr rather than to the table: cmp w0,#5; b.hi; ...; adr x18,#L; ldrsw x1,[x17,x0,lsl #2];
// add x18,x18,x1; br x18 -> target = L + entry, the table base x17 from adrp + add. It is tried only where JumpOffsets
// finds nothing (jump_adr_offsets).
std::vector<Addr> Rewriter::JumpAdrOffsets(const InsnList& insl, std::size_t ju) const {
    const auto j = static_cast<Index>(ju);
    const Index c0 = std::max<Index>(0, j - 16);
    Index ka = -1;
    std::string_view op1, op2;
    for (Index i = j - 1; i >= c0 && ka < 0; --i) {
        if (insl[i].Is("add") && Scan(insl[i].op_str, "x18, %x, %x", {&op1, &op2})) ka = i;
    }
    if (ka < 0) return {};
    for (const auto& [e, a] : {std::pair(op1, op2), std::pair(op2, op1)}) {  // e = the loaded entry, a = the adr base
        Index kl = -1;
        std::string_view tb_reg, ix;
        const std::string load = absl::StrCat(e, ", [%x, x%n, lsl #2]");
        for (Index i = ka - 1; i >= c0 && kl < 0; --i) {
            if (insl[i].Is("ldrsw") && Scan(insl[i].op_str, load, {&tb_reg, &ix})) kl = i;
        }
        Index wa = -1;  // the nearest writer of a
        const int an = NumberOf(a);
        for (Index i = ka - 1; i >= c0 && wa < 0; --i) {
            if (WritesReg(insl[i], an)) wa = i;
        }
        if (kl < 0 || wa < 0 || !insl[wa].Is("adr")) continue;
        std::uint32_t idx = Bit(static_cast<int>(ParseInt(ix)));  // the index and the registers copied into it
        Index cmp = -1;
        Addr n = 0;
        for (Index i = kl - 1; i >= c0; --i) {  // walk back from the load to the bounds check
            const Insn& p = insl[i];
            std::string_view x, y;
            if (p.Is("mov") && Scan(p.op_str, "%w, %w", {&x, &y}) && (BitOf(x) & idx) != 0) {
                idx = (idx & ~BitOf(x)) | BitOf(y);
                continue;
            }
            // A check of the full xN index bounds the same table; only a compare a branch acts on bounds the index
            if (p.Is("cmp") && Scan(p.op_str, "%r, #%i", {&x}) && (BitOf(x) & idx) != 0) {
                const std::optional<Addr> g = GuardedCount(insl, i, kl);
                if (!g) continue;
                cmp = i;
                n = *g;
                break;
            }
            if ((WrittenMask(p) & idx) != 0) return {};  // the index changes after the check
        }
        if (cmp < 0) return {};
        const Addr at = insl[wa].operands[1].imm;
        const auto targets = [&](Addr tb) {
            if (!(0 <= tb && tb + 4 * n <= static_cast<Addr>(img_.size()))) return std::vector<Addr>();
            return ReadTable(tb, n, at);
        };
        const std::string base(tb_reg);
        if (const std::optional<Addr> tb = TableBase(insl, kl, base, false)) return targets(*tb);
        // The base register was loaded further away (hoisted out of the dispatch block): take every adrp + add pair in
        // the function that builds it, and accept the table only if exactly one candidate makes every target an
        // instruction of this function
        const Addr fa = insl.front().address;
        const Addr fb = insl.back().address + 4;
        std::vector<Addr> ok;
        for (const Addr tb : TableCands(insl, base)) {
            const std::vector<Addr> got = targets(tb);
            if (!got.empty() &&
                std::all_of(got.begin(), got.end(), [&](Addr x) { return fa <= x && x < fb && x % 4 == 0; })) {
                ok.push_back(tb);
            }
        }
        return ok.size() == 1 ? targets(ok[0]) : std::vector<Addr>();
    }
    return {};
}

// The offset-table form whose bounds check is on a masked copy of the index made in the block itself, while the index
// register was masked in the one block that branches here. The two ands give the same value (a mask below 2^32 makes
// the 64-bit and equal to the zero-extended 32-bit one), so the check bounds the index. Accepted only when the block,
// at most 32 instructions, is and wC,wS,#M; cmp wC,#N; b.hi, then straight-line code holding the ldrsw and the add,
// with the index unchanged from the b.hi to the load, the base unchanged and x18 untouched from the load to the add and
// x18 untouched from the add to the br; the block is entered by branches alone, and every branch to it follows, within
// its own straight-line run, "and xI, xS, #M" with the check's source and mask and neither register written in
// between. Tried only after every other form has failed (jump_masked_pred).
std::vector<Addr> Rewriter::JumpMaskedPred(const InsnList& insl, std::size_t ju) const {
    const auto j = static_cast<Index>(ju);
    const auto size = static_cast<Index>(insl.size());
    // Capstone lists a compare's first operand as written; it only sets the flags
    const auto writes = [](const Insn& p, int reg) {
        if (IsCompare(p)) return false;
        for (const std::uint16_t r : p.regs_write) {
            if (NumOf(p.Name(r)) == reg) return true;
        }
        return false;
    };
    Index kh = -1;  // the block's b.hi: the nearest control flow before the br
    for (Index q = j - 1; q >= std::max<Index>(0, j - 32) && kh < 0; --q) {
        if (IsControlFlow(insl[q])) kh = q;
    }
    if (kh < 3 || !insl[kh].Is("b.hi")) return {};
    const Index k = kh - 2;
    const Insn& a_ = insl[kh - 2];
    const Insn& c_ = insl[kh - 1];
    std::string_view and_to, and_from, and_imm, cmp_reg, cmp_imm;
    if (!(a_.Is("and") && Scan(a_.op_str, "%w, %w, #%i", {&and_to, &and_from, &and_imm})) ||
        !(c_.Is("cmp") && Scan(c_.op_str, "%w, #%i", {&cmp_reg, &cmp_imm})) || and_to != cmp_reg) {
        return {};
    }
    if (!Contiguous(insl, k - 1, j)) return {};  // an undecodable word may hide a write or a branch
    Index kl = -1;
    for (Index q = kh + 1; q < j && kl < 0; ++q) {
        if (insl[q].Is("ldrsw") && insl[q].op_str.starts_with("x18, ")) kl = q;
    }
    std::string_view rb_name, ix_text;
    if (kl < 0 || !Scan(insl[kl].op_str, "x18, [%x, x%n, lsl #2]", {&rb_name, &ix_text})) return {};
    const std::string rb(rb_name);
    const int rbn = NumberOf(rb);
    const int ix = static_cast<int>(ParseInt(ix_text));
    const int src = NumberOf(and_from);
    const Addr mask = ParseInt(and_imm);
    const Addr n = ParseInt(cmp_imm) + 1;
    if (ix == src || ix == NumberOf(and_to) || ix == 18 || rbn == ix || rbn == 18 || (mask >> 32) != 0) return {};
    for (Index q = kh + 1; q < kl; ++q) {
        if (writes(insl[q], ix)) return {};  // the index changes between the check and the load
    }
    Index ka = -1;  // the first write of x18 after the load must be the add
    for (Index q = kl + 1; q < j && ka < 0; ++q) {
        if (writes(insl[q], 18)) ka = q;
    }
    if (ka < 0 || !insl[ka].Is("add") || insl[ka].op_str != absl::StrCat("x18, x18, ", rb)) return {};
    for (Index q = kl + 1; q < ka; ++q) {
        if (writes(insl[q], rbn)) return {};
    }
    for (Index q = ka + 1; q < j; ++q) {
        if (writes(insl[q], 18)) return {};
    }
    if (!insl[k - 1].IsAny("b", "br", "ret")) return {};  // no fall-through into the block
    std::vector<Index> brs;
    absl::flat_hash_set<Addr> into;
    for (Index q = 0; q < size; ++q) {
        const Insn& p = insl[q];
        if ((IsJump(p) || p.Is("bl")) && p.operands[p.op_count - 1].type == ARM64_OP_IMM) {
            brs.push_back(q);
            into.insert(p.Target());
        }
    }
    for (Index q = k + 1; q <= j; ++q) {
        if (into.contains(insl[q].address)) return {};  // a branch into the block past its and
    }
    // A jump table already resolved in this function may land anywhere in the block, with any index value
    absl::flat_hash_set<Addr> tabs;
    for (const auto& [at, targets] : tabjumps_) tabs.insert(targets.begin(), targets.end());
    const auto table_target = [&](Addr a) {
        if (tabs.contains(a)) return true;
        for (const auto& [at, table] : jumps_) {
            if (table.first <= a && a < table.first + 4 * table.second) return true;
        }
        return false;
    };
    for (Index q = k; q <= j; ++q) {
        if (table_target(insl[q].address)) return {};
    }
    std::vector<Index> preds;
    for (const Index q : brs) {
        if (insl[q].Target() == a_.address) preds.push_back(q);
    }
    if (preds.empty()) return {};
    for (const Index q : preds) {
        if (insl[q].Is("bl")) return {};
    }
    for (const Index q : preds) {  // each predecessor must have masked the index the same way, in its own run
        bool found = false;
        for (Index i = q - 1; i >= std::max<Index>(0, q - 16); --i) {
            const Insn& p = insl[i];
            std::string_view to, from, imm;
            if (p.Is("and") && Scan(p.op_str, "%r, %r, #%i", {&to, &from, &imm}) && NumberOf(to) == ix) {
                if (NumberOf(from) != src || ParseInt(imm) != mask) return {};
                for (Index x = i + 1; x <= q; ++x) {  // a direct branch, call or table must not bypass the mask
                    if (into.contains(insl[x].address) || table_target(insl[x].address)) return {};
                }
                if (!Contiguous(insl, i, q)) return {};
                found = true;
                break;
            }
            if (IsControlFlow(p) || writes(p, ix) || writes(p, src)) return {};
        }
        if (!found) return {};
    }
    const std::optional<Addr> tb = CopiedBase(insl, kl, rb);  // follows adrp + add too
    if (!tb || *tb < 0 || *tb + 4 * n > static_cast<Addr>(img_.size())) return {};
    std::vector<Addr> targets = ReadTable(*tb, n, *tb);
    absl::flat_hash_set<Addr> addresses;
    for (const Insn& p : insl) addresses.insert(p.address);
    for (const Addr t : targets) {
        if (!addresses.contains(t)) return {};
    }
    return targets;
}

// The byte / halfword table form with its add further back: adr x18,#L; ldrb w0,[x17,x15]; add x18,x18,x0,lsl #2, then
// register moves before br x18. Up to 32 instructions back, and x18 must not change between the add and the br. Tried
// only where the 16-instruction search and the offset forms find nothing. -> (base, width) (jump_bytes_far)
std::optional<std::pair<Addr, Addr>> Rewriter::JumpBytesFar(const InsnList& insl, std::size_t ju) const {
    const auto j = static_cast<Index>(ju);
    const Index c0 = std::max<Index>(0, j - 32);
    Index k = -1;
    for (Index i = j - 1; i >= c0; --i) {
        const Insn& p = insl[i];
        if (p.Is("add") && p.op_str.starts_with("x18, x18,") && absl::StrContains(p.op_str, "lsl #2")) {
            k = i;
            break;
        }
        if (WritesReg(p, 18)) return std::nullopt;
    }
    if (k < 0) return std::nullopt;
    const std::optional<int> idx = NumOf(insl[k].OpReg(2));
    std::optional<Addr> base;
    for (Index i = k - 1; i >= c0 && !base; --i) {
        if (insl[i].Is("adr") && insl[i].op_str.starts_with("x18,")) base = insl[i].operands[1].imm;
    }
    Index ld = -1;
    for (Index i = k - 1; i >= c0 && ld < 0; --i) {
        if (insl[i].IsAny("ldrb", "ldrsb", "ldrh", "ldrsh") && RegNum(insl[i].OpReg(0)) == idx) ld = i;
    }
    if (!base || ld < 0) return std::nullopt;
    if (insl[ld].IsAny("ldrb", "ldrsb")) return std::pair<Addr, Addr>(*base, 256);
    return std::pair<Addr, Addr>(*base, HalfCount(insl, static_cast<std::size_t>(ld), *base).value_or(65536));
}

// The landing pads a halfword table needs (ldrh wE,[xT,xI,lsl #1]; ...; adr x18,#L; add x18,x18,xE,lsl #2; br x18:
// target = L + 4 x entry). Every possible entry would take 65,536 pads (768 KB), whatever the switch's size; the table
// is read as JumpAdrOffsets reads one -> the largest entry + 1, or nullopt (all 65,536) (half_count).
std::optional<Addr> Rewriter::HalfCount(const InsnList& insl, std::size_t ldu, Addr base) const {
    const auto k = static_cast<Index>(ldu);
    const Insn& ld = insl[k];
    std::string_view tb_name, ix;
    if (!ld.Is("ldrh")) return std::nullopt;
    bool shape = false;
    for (const char* pattern :
         {"%w, [%x, w%n, lsl #1]", "%w, [%x, x%n, lsl #1]", "%w, [%x, w%n, uxtw #1]", "%w, [%x, x%n, uxtw #1]"}) {
        std::string_view value;
        shape = shape || Scan(ld.op_str, pattern, {&value, &tb_name, &ix});
    }
    if (!shape) return std::nullopt;
    const std::string tb_reg(tb_name);
    std::uint32_t idx = Bit(static_cast<int>(ParseInt(ix)));
    Index kc = -1;
    for (Index i = k - 1; i >= std::max<Index>(0, k - 32); --i) {  // walk back from the load to the bounds check
        const Insn& p = insl[i];
        std::string_view x, y;
        if (p.Is("mov") && Scan(p.op_str, "%w, %w", {&x, &y}) && (BitOf(x) & idx) != 0) {
            idx = (idx & ~BitOf(x)) | BitOf(y);  // a copy replaces the index
            continue;
        }
        if (p.Is("cmp") && Scan(p.op_str, "%w, #%i", {&x}) && (BitOf(x) & idx) != 0) {
            kc = i;
            break;
        }
        if ((WrittenMask(p) & idx) != 0) return std::nullopt;  // the index changes after the check
    }
    // Only a compare that guards the dispatch bounds the index (b.hs: one entry more than needed)
    if (kc < 0 || !insl[kc + 1].IsAny("b.hi", "b.hs", "b.cs")) return std::nullopt;
    const Addr n = ImmAfterHash(insl[kc]) + 1;
    const Addr fa = insl.front().address;
    const Addr fb = insl.back().address + 4;
    const auto size = static_cast<Addr>(img_.size());
    // The table's entries, when they all lead into the function: the largest + 1
    const auto count = [&](Addr tb) -> std::optional<Addr> {
        if (!(0 <= tb && tb + 2 * n <= size)) return std::nullopt;
        Addr largest = -1;
        for (Addr i = 0; i < n; ++i) {
            const Addr entry = Load<std::uint16_t>(img_, tb + 2 * i);
            if (!(fa <= base + 4 * entry && base + 4 * entry < fb)) return std::nullopt;
            largest = std::max(largest, entry);
        }
        if (largest < 0) Fail(absl::StrFormat("halfword jump table at %s has no entries", Hex(ld.address)));
        return largest + 1;
    };
    std::optional<Addr> tb = TableBase(insl, k, tb_reg, false);
    if (!tb) {
        std::vector<Addr> ok;
        for (const Addr c : TableCands(insl, tb_reg)) {
            if (count(c)) ok.push_back(c);
        }
        if (ok.size() == 1) tb = ok[0];
    }
    return tb ? count(*tb) : std::nullopt;
}

InsnList Rewriter::DisasmFn(Addr fa, Addr fb) const {
    // A function's instructions. Capstone stops at the first word it cannot decode, so go on after it: a trap word in
    // the middle of a function must not hide the br x18 after it.
    InsnList out;
    Addr a = fa;
    while (a < fb) {
        InsnList got =
            md_.Disasm(Bytes(img_).subspan(static_cast<std::size_t>(a), static_cast<std::size_t>(fb - a)), a);
        a = (got.empty() ? a : got.back().address + 4) + 4;
        out.insert(out.end(), std::make_move_iterator(got.begin()), std::make_move_iterator(got.end()));
    }
    return out;
}

// [start, end) pieces covering [fa, fb), cut only right after an unconditional ret / br / b where no conditional branch
// and no b within kPieceB crosses; *far gets the b / bl that may cross a cut, as (site, target) (pieces)
std::vector<Range> Rewriter::Pieces(Addr fa, Addr fb, std::vector<Range>* far) const {
    const Addr n = (fb - fa) / 4;
    std::vector<std::int32_t> cross(static_cast<std::size_t>(n) + 2, 0);
    for (Addr i = 0; i < n; ++i) {
        const std::uint32_t x = Word(fa + 4 * i);
        const std::uint32_t t = x >> 24;
        Addr d;
        if ((t == 0x54 && (x & 0x10) == 0) || (t & 0x7E) == 0x34) {  // b.cond, cbz / cbnz
            d = (x >> 5) & 0x7FFFF;
            d -= (d & 0x40000) << 1;
        } else if ((t & 0x7E) == 0x36) {  // tbz / tbnz
            d = (x >> 5) & 0x3FFF;
            d -= (d & 0x2000) << 1;
        } else if ((t & 0x7C) == 0x14) {  // b, bl
            d = x & 0x3FFFFFF;
            d -= (d & 0x2000000) << 1;
            if ((t & 0x80) != 0 || 4 * std::llabs(d) > kPieceB) {
                far->emplace_back(fa + 4 * i, fa + 4 * (i + d));
                continue;
            }
        } else {
            continue;
        }
        // The branch crosses the cut before instruction k when lo < k <= hi
        const Addr lo = std::max<Addr>(std::min(i, i + d), -1);
        const Addr hi = std::min<Addr>(std::max(i, i + d), n);
        ++cross[static_cast<std::size_t>(lo + 1)];
        --cross[static_cast<std::size_t>(hi + 1)];
    }
    std::vector<Addr> cand;
    std::int64_t c = 0;
    for (Addr k = 0; k < n; ++k) {
        c += cross[static_cast<std::size_t>(k)];
        const std::uint32_t x = k != 0 ? Word(fa + 4 * (k - 1)) : 0;
        // After ret / br / b, nothing crossing
        if (k != 0 && c == 0 &&
            ((x & 0xFFFFFC1F) == 0xD65F0000 || (x & 0xFFFFFC1F) == 0xD61F0000 || (x >> 26) == 0x05)) {
            cand.push_back(k);
        }
    }
    std::vector<Addr> cuts;
    Addr s = 0;
    while (s + kPiece / 4 < n) {
        auto j = std::upper_bound(cand.begin(), cand.end(), s + kPiece / 4) - cand.begin() - 1;
        if (j < 0 || cand[static_cast<std::size_t>(j)] <= s) {
            j = std::upper_bound(cand.begin(), cand.end(), s + kPiece / 4) - cand.begin();
        }
        if (j >= static_cast<Index>(cand.size())) break;
        s = cand[static_cast<std::size_t>(j)];
        cuts.push_back(s);
    }
    std::vector<Range> out;
    Addr from = 0;
    for (const Addr cut : cuts) {
        out.emplace_back(fa + 4 * from, fa + 4 * cut);
        from = cut;
    }
    out.emplace_back(fa + 4 * from, fa + 4 * n);
    return out;
}

void Rewriter::AnalyseFunctions() {
    std::set<Range> func_set;
    std::vector<Addr> sites18;
    for (const auto& [a, site] : kinds_) {
        if (site.kind != Kind::kX18) continue;
        func_set.insert(FuncOf(a));
        sites18.push_back(a);
    }
    std::vector<Range> funcs(func_set.begin(), func_set.end());
    std::vector<Range> far_into;
    absl::flat_hash_set<Addr> rmid_all;
    absl::flat_hash_set<Addr> newly_accepted;
    if (std::any_of(funcs.begin(), funcs.end(), [](const Range& f) { return f.second - f.first > kBigFn; })) {
        std::vector<Range> split;
        for (const auto& [fa, fb] : funcs) {
            if (fb - fa <= kBigFn) {
                split.emplace_back(fa, fb);
                continue;
            }
            const std::vector<Range> pieces = Pieces(fa, fb, &far_into);
            for (const auto& [p, q] : pieces) {  // the pieces that hold x18 sites
                if (std::lower_bound(sites18.begin(), sites18.end(), p) <
                    std::lower_bound(sites18.begin(), sites18.end(), q)) {
                    split.emplace_back(p, q);
                }
            }
            Log(absl::StrFormat("function %s..%s (%.2f MB, no unwind entries) analysed in %d pieces", Hex(fa), Hex(fb),
                                static_cast<double>(fb - fa) / 1048576.0, pieces.size()));
        }
        funcs = std::move(split);
    }
    for (const auto& [fa, fb] : funcs) {
        // Only an exclusive load (a window) or a br x18 gives a function anything to find below, and both show in the
        // words themselves: the other functions, most of them, are not decoded
        bool worth = false;
        for (Addr a = fa; a + 4 <= fb && !worth; a += 4) {
            const std::uint32_t w = Word(a);
            worth = w == 0xD61F0240 || (w & 0x3FC00000) == 0x08400000;
        }
        if (!worth) continue;
        const InsnList insl = DisasmFn(fa, fb);
        const std::vector<InsnList> ws = FindWindows(insl);
        for (const InsnList& wseg : ws) {
            windows_.push_back(wseg);
            window_ranges_.emplace_back(wseg.front().address, wseg.back().address + 4);
            for (const Insn& x : wseg) done_.insert(x.address);
            if (std::any_of(wseg.begin(), wseg.end(), [](const Insn& x) { return x.Is("b"); })) {
                newly_accepted.insert(wseg.front().address);
            }
        }
        for (std::size_t j = 0; j < insl.size(); ++j) {
            const Insn& ins = insl[j];
            if (!ins.Is("br") || ins.op_str != "x18") continue;
            std::optional<Addr> base;
            Addr width = 0;
            // 16 instructions back, as for the offset-table form
            const Index c0 = std::max<Index>(0, static_cast<Index>(j) - 16);
            for (Index i = static_cast<Index>(j) - 1; i >= c0 && !base; --i) {
                if (insl[i].Is("adr") && insl[i].op_str.starts_with("x18,")) base = insl[i].operands[1].imm;
            }
            for (Index i = static_cast<Index>(j) - 1; i >= c0; --i) {
                const Insn& p = insl[i];
                if (!(p.Is("add") && p.op_str.starts_with("x18, x18,") && absl::StrContains(p.op_str, "lsl #2"))) {
                    continue;
                }
                // Capstone names x30 lr (and x29 fp)
                const std::optional<int> idx = NumOf(p.OpReg(2));
                for (Index q = static_cast<Index>(j) - 1; q >= c0 && width == 0; --q) {
                    if (RegNum(insl[q].OpReg(0)) != idx || insl[q].op_count == 0) continue;
                    if (insl[q].IsAny("ldrb", "ldrsb")) width = 256;
                    if (insl[q].IsAny("ldrh", "ldrsh")) {
                        width = base && *base != 0 ? HalfCount(insl, static_cast<std::size_t>(q), *base).value_or(65536)
                                                   : 65536;
                    }
                }
                break;
            }
            if (base && *base != 0 && width != 0) {
                jumps_[ins.address] = {*base, width};
                continue;
            }
            std::vector<Addr> tgt = JumpOffsets(insl, j, false, false, false);
            if (tgt.empty()) tgt = JumpAdrOffsets(insl, j);
            if (tgt.empty() && TailCall(insl, j)) {
                tailcalls_.insert(ins.address);
                continue;
            }
            if (tgt.empty()) {
                if (const auto far = JumpBytesFar(insl, j)) {
                    jumps_[ins.address] = *far;
                    continue;
                }
            }
            // Only where every other form failed: the index copied before the check or the base set far back
            if (tgt.empty()) tgt = JumpOffsets(insl, j, true, true, false);
            if (tgt.empty()) tgt = JumpMaskedPred(insl, j);
            if (tgt.empty()) tgt = JumpMaskedNot(insl, j);
            if (tgt.empty()) tgt = JumpOffsets(insl, j, true, true, true);
            if (tgt.empty()) tgt = JumpZeroExtended(insl, j);
            if (tgt.empty()) Fail(absl::StrFormat("jump table %s", HexBare(ins.address)));
            tabjumps_[ins.address] = std::move(tgt);
        }
        // No jump may land in the middle of a window, except a retry block's branch onto the window's store-exclusive,
        // which gets a copy of its own
        absl::flat_hash_map<Addr, std::size_t> inwin;  // address -> its window
        absl::flat_hash_set<Addr> heads;
        for (std::size_t w = 0; w < ws.size(); ++w) {
            for (const Insn& x : ws[w]) inwin.emplace(x.address, w);
            heads.insert(ws[w].front().address);
        }
        absl::flat_hash_set<Addr> rmid;
        for (std::size_t k = 0; k < insl.size(); ++k) {
            const Insn& x = insl[k];
            if (!IsDirect(x)) continue;
            const Addr t = x.Target();
            const auto hit = inwin.find(t);
            if (hit == inwin.end() || heads.contains(t)) continue;
            const InsnList& wseg = ws[hit->second];
            if (!x.Is("bl") && x.address >= wseg.front().address &&
                std::any_of(wseg.begin(), wseg.end(), [&](const Insn& y) { return y.address == x.address; })) {
                newly_accepted.insert(wseg.front().address);
                continue;
            }
            std::optional<std::pair<InsnList, InsnList>> rb;
            if (!x.IsAny("b", "bl")) rb = RetryBlock(insl, k, wseg);
            if (!rb) Fail(absl::StrFormat("branch into window %s", HexBare(x.address)));
            for (const Insn& y : rb->first) {
                if (done_.contains(y.address)) {
                    Fail(absl::StrFormat("retry block overlaps a window %s", HexBare(rb->first.front().address)));
                }
            }
            absl::flat_hash_set<Addr> tail_addrs;
            for (const Insn& y : rb->second) tail_addrs.insert(y.address);
            for (const Insn& y : rb->second) {
                if (!(IsCbTb(y) || IsBCond(y))) continue;
                const Addr dest = y.Target();
                if (inwin.contains(dest) && !heads.contains(dest) && !tail_addrs.contains(dest)) {
                    Fail(absl::StrFormat("retry copy branches into window %s", HexBare(y.address)));
                }
            }
            for (std::size_t y = 0; y < rb->first.size(); ++y) {
                done_.insert(rb->first[y].address);
                if (y != 0) rmid.insert(rb->first[y].address);
            }
            retries_.push_back({RetryPart{std::move(rb->first), x.address + 4},
                                RetryPart{std::move(rb->second), wseg.back().address + 4}});
        }
        for (const Insn& x : insl) {
            if (IsDirect(x) && rmid.contains(x.Target())) {
                Fail(absl::StrFormat("branch into retry block %s", HexBare(x.address)));
            }
        }
        for (auto it = tabjumps_.lower_bound(fa); it != tabjumps_.end() && it->first < fb; ++it) {
            for (const Addr t : it->second) {
                if (inwin.contains(t) && !heads.contains(t)) {
                    Fail(absl::StrFormat("jump table target into window %s", HexBare(it->first)));
                }
            }
        }
        rmid_all.insert(rmid.begin(), rmid.end());
    }
    if (!far_into.empty()) {
        // Functions analysed in pieces: their far b / bl, and every jump table's targets (a cut may follow a br x18),
        // may not land inside a window or a retry block
        absl::flat_hash_set<Addr> win_mid;
        for (const InsnList& wseg : windows_) {
            for (std::size_t i = 1; i < wseg.size(); ++i) win_mid.insert(wseg[i].address);
        }
        for (const auto& [at, to] : far_into) {
            if (win_mid.contains(to) || rmid_all.contains(to)) {
                Fail(absl::StrFormat("far branch into a window or retry block %s", HexBare(at)));
            }
        }
        for (const auto& [at, targets] : tabjumps_) {
            for (const Addr t : targets) {
                if (win_mid.contains(t) || rmid_all.contains(t)) {
                    Fail(absl::StrFormat("jump table target into a window or retry block %s", HexBare(at)));
                }
            }
        }
    }
    // Direct branches from other functions were not part of the per-function entry check above
    absl::flat_hash_set<Addr> new_mid;
    for (const InsnList& w : windows_) {
        if (!newly_accepted.contains(w.front().address)) continue;
        for (std::size_t i = 1; i < w.size(); ++i) {
            if (w[i].address != w[i - 1].address + 4) {
                Fail(absl::StrFormat("noncontiguous window %s", HexBare(w.front().address)));
            }
            new_mid.insert(w[i].address);
        }
    }
    if (!new_mid.empty()) {
        for (const auto& [start, end] : code_ranges_) {
            for (Addr at = start; at < end - 3; at += 4) {
                const std::uint32_t w = Word(at);
                const std::uint32_t t = w >> 24;
                Addr d;
                if ((t == 0x54 && (w & 0x10) == 0) || (t & 0x7E) == 0x34) {
                    d = (w >> 5) & 0x7FFFF;
                    d -= (d & 0x40000) << 1;
                } else if ((t & 0x7E) == 0x36) {
                    d = (w >> 5) & 0x3FFF;
                    d -= (d & 0x2000) << 1;
                } else if ((t & 0x7C) == 0x14) {
                    d = w & 0x3FFFFFF;
                    d -= (d & 0x2000000) << 1;
                } else {
                    continue;
                }
                const Addr dest = at + 4 * d;
                if (new_mid.contains(dest) && FuncOf(at) != FuncOf(dest)) {
                    Fail(absl::StrFormat("branch into window from another function %s", HexBare(at)));
                }
            }
        }
    }
}

}  // namespace nxmac::rewriter::internal
