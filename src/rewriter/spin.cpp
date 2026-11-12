// Spin waits (patch_nso.py "Spin waits"). A guest loop that polls memory until another thread changes it fills only
// its own core on the Switch, where any higher-priority thread pinned there preempts it; on the host it takes a whole
// core from the threads doing the work. A loop counts as a spin wait only when it provably cannot leave by itself:
// from its head T, every pass along the region R (the paths from T back to T, within 512 bytes after T) runs only
// loads, register and flag arithmetic, direct branches and barriers, and no register or flag that is live at T is
// written in R. Exceptions that leave memory as a pass found it: an exclusive add of a constant that every path back
// to T undoes on the same address; a spin lock taken and released on the same address on every path; a test loop on a
// word inside R; and at most kSpinStores stores of fixed values to fixed addresses, beside a load. R is copied into
// the patch area; the original back-edges jump to the copy (no pause on the first pass), the copy's back-edges go
// through a spin gate, which the host takes as a yield, and exits from the copy go back to the original code.
#include <algorithm>
#include <exception>

#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "rewriter_impl.h"
#include "text.h"

namespace nxmac::rewriter::internal {
namespace {

using namespace a64;  // NOLINT: the encoders

constexpr int kSpinPaths = 256;   // complete paths back to T
constexpr int kSpinSteps = 4096;  // search steps
constexpr int kSpinStores = 4;

// The search gave up: too many paths, an inner cycle, a lock not taken once (SpinNo)
struct SpinNo : std::exception {};

using Names = std::set<std::string>;

// A register's name with w / x and the vector views merged; nullopt for the zero register (spin_norm)
std::optional<std::string> SpinNorm(std::string_view n) {
    if (n == "fp") n = "x29";
    if (n == "lr") n = "x30";
    if (n == "wsp") n = "sp";
    if (n == "wzr" || n == "xzr") return std::nullopt;
    if (const std::optional<int> num = RegNum(n)) return "x" + std::to_string(*num);
    if (n.size() > 1 && absl::StrContains("bhsdqv", n[0]) &&
        std::all_of(n.begin() + 1, n.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return "v" + std::string(n.substr(1));
    }
    return std::string(n);
}

void Add(Names* names, const std::optional<std::string>& name) {
    if (name) names->insert(*name);
}

// (registers read, registers written). Capstone's own lists are not enough: it lists a compare's first operand as
// written and the flags as read by cbz / tbz, and leaves out the destination of adds. So every register operand counts
// as well: operand 0 written and the others read, or for a load all of them written (spin_rw).
void SpinRw(const Insn& ins, Names* uses, Names* defs) {
    std::vector<std::optional<std::string>> regs;
    for (std::size_t i = 0; i < ins.op_count; ++i) {
        if (ins.operands[i].type == ARM64_OP_REG) regs.push_back(SpinNorm(ins.Name(ins.operands[i].reg)));
    }
    Names ops;
    for (const auto& r : regs) Add(&ops, r);
    const std::string_view m = ins.mnemonic;
    if (IsCbTb(ins)) {
        *uses = ops;
        return;
    }
    if (IsBCond(ins)) {
        uses->insert("nzcv");
        return;
    }
    if (m == "b") return;
    if (ins.IsAny("cmp", "cmn", "tst", "fcmp", "fcmpe")) {
        *uses = ops;
        defs->insert("nzcv");
        return;
    }
    if (ins.IsAny("ccmp", "ccmn", "fccmp", "fccmpe")) {
        *uses = ops;
        uses->insert("nzcv");
        defs->insert("nzcv");
        return;
    }
    if (!ins.regs_ok) Fail(absl::StrFormat("Capstone lists no registers at %s", Hex(ins.address)));
    for (const std::uint16_t r : ins.regs_read) Add(uses, SpinNorm(ins.Name(r)));
    for (const std::uint16_t r : ins.regs_write) Add(defs, SpinNorm(ins.Name(r)));
    if (m.starts_with("ld") || m.starts_with("prfm") || m.starts_with("prfum")) {
        defs->insert(ops.begin(), ops.end());
        return;
    }
    for (std::size_t i = 0; i < regs.size(); ++i) Add(i == 0 ? defs : uses, regs[i]);
}

bool StartsWithAny(std::string_view m, std::initializer_list<std::string_view> prefixes) {
    return std::any_of(prefixes.begin(), prefixes.end(), [&](std::string_view p) { return m.starts_with(p); });
}

// A direct branch, adr / adrp, or (kOk) a load or register / flag arithmetic; nullopt for anything else (spin_kind)
std::optional<SpinNode::Type> SpinKind(const Insn* ins) {
    if (ins == nullptr) return std::nullopt;
    const std::string_view m = ins->mnemonic;
    if (m == "b" || IsCbTb(*ins) || IsBCond(*ins)) return SpinNode::kBranch;
    if (m == "adr" || m == "adrp") return SpinNode::kPc;
    if (ins->IsAny("bl", "blr", "br", "ret", "eret", "svc", "hvc", "smc", "brk", "hlt", "udf", "mrs", "msr", "sys",
                   "sysl", "wfe", "wfi", "sev", "sevl", "clrex", "dc", "ic", "at", "tlbi", "drps", "dcps1", "dcps2",
                   "dcps3", "blraa", "blrab", "blraaz", "blrabz", "braa", "brab", "braaz", "brabz", "retaa", "retab",
                   "eretaa", "eretab") ||
        StartsWithAny(
            m, {"st", "cas", "swp", "ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin", "ldumax", "ldumin"})) {
        return std::nullopt;
    }
    bool mem = false;
    for (std::size_t i = 0; i < ins->op_count; ++i) mem = mem || ins->operands[i].type == ARM64_OP_MEM;
    // A load without a memory operand is a pc-relative literal load
    if (StartsWithAny(m, {"ld", "prfm", "prfum"})) return mem ? std::optional(SpinNode::kOk) : std::nullopt;
    return mem ? std::nullopt : std::optional(SpinNode::kOk);
}

// str / strb / strh / stur / sturb / sturh of a w register or zr to [xN{,#imm}] (no writeback, no register offset)
// -> (value register or none for zr, base register) (spin_store)
struct Store {
    std::optional<std::string> value;
    std::string base;
};
std::optional<Store> SpinStore(const Insn* ins) {
    if (ins == nullptr || !ins->IsAny("str", "strb", "strh", "stur", "sturb", "sturh") || ins->writeback ||
        ins->op_count != 2) {
        return std::nullopt;
    }
    const cs_arm64_op& v = ins->operands[0];
    const cs_arm64_op& o = ins->operands[1];
    if (v.type != ARM64_OP_REG || o.type != ARM64_OP_MEM || o.mem.index != 0) return std::nullopt;
    const std::string_view vn = ins->Name(v.reg);
    if (!(vn.starts_with('w') || vn == "xzr")) return std::nullopt;
    return Store{SpinNorm(vn), SpinNorm(ins->Name(o.mem.base)).value_or("")};
}

// The direct branch ins re-encoded at pc with target tgt (spin_rebranch)
std::uint32_t SpinRebranch(const Insn& ins, Addr pc, Addr tgt) {
    const std::uint32_t w = ins.word;
    const Addr off = tgt - pc;
    if (ins.Is("b")) return B(pc, tgt);
    if (ins.IsAny("tbz", "tbnz")) {
        if (!(-(Addr{1} << 15) <= off && off < (Addr{1} << 15) && off % 4 == 0)) {
            Fail(absl::StrFormat("spin wait tbz out of range %s", Hex(pc)));
        }
        return (w & ~(0x3FFFU << 5)) | static_cast<std::uint32_t>((off >> 2) & 0x3FFF) << 5;
    }
    if (!(-(Addr{1} << 20) <= off && off < (Addr{1} << 20) && off % 4 == 0)) {
        Fail(absl::StrFormat("spin wait branch out of range %s", Hex(pc)));
    }
    return (w & ~(0x7FFFFU << 5)) | static_cast<std::uint32_t>((off >> 2) & 0x7FFFF) << 5;
}

bool PlainMem(const Insn& x, int i) {  // [xN]: no index, no displacement
    const cs_arm64_op& o = x.operands[static_cast<std::size_t>(i)];
    return o.type == ARM64_OP_MEM && o.mem.index == 0 && o.mem.disp == 0;
}

}  // namespace

const Insn* Rewriter::SpinIns(Addr a) {
    const auto [it, added] = spin_dec_.try_emplace(a);
    if (added && a >= 0 && static_cast<std::uint64_t>(a) + 4 <= img_.size()) it->second = md_.One(Word(a), a);
    return it->second ? &*it->second : nullptr;
}

bool Rewriter::SpinQuick(Addr head, Addr hi) const {
    // A back-edge to T can be reached from T without passing an instruction word that ends every path through it, told
    // apart by its encoding alone: bl; br / blr / ret; svc / brk; str / strb / strh and the vector str; stp / stnp;
    // ldadd and the other LSE atomics. Only such heads get the full check, which decodes (spin_quick).
    static constexpr std::pair<std::uint32_t, std::uint32_t> kStop[] = {
        {0xFC000000, 0x94000000}, {0xFE000000, 0xD6000000}, {0xFF000000, 0xD4000000}, {0x3FC00000, 0x39000000},
        {0x3FC00000, 0x3D000000}, {0x3FC00000, 0x3D800000}, {0x3FE00000, 0x38000000}, {0x3FE00C00, 0x38200800},
        {0x3FE00000, 0x3C000000}, {0x3FE00000, 0x3C800000}, {0x3A400000, 0x28000000}, {0x3F200C00, 0x38200000}};
    absl::flat_hash_set<Addr> seen;
    std::vector<Addr> todo{head};
    while (!todo.empty()) {
        const Addr a = todo.back();
        todo.pop_back();
        if (seen.contains(a) || !(head <= a && a <= hi)) continue;
        seen.insert(a);
        const std::uint32_t w = Word(a);
        const std::uint32_t t = w >> 24;
        Addr succ[2] = {a + 4, a + 4};
        int count = 1;
        if (0x14 <= t && t <= 0x17) {
            succ[0] = a + (static_cast<Addr>((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) * 4;
        } else if (t == 0x54 || (t & 0x7E) == 0x34) {
            succ[0] = a + (static_cast<Addr>(((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000) * 4;
            count = 2;
        } else if ((t & 0x7E) == 0x36) {
            succ[0] = a + (static_cast<Addr>(((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000) * 4;
            count = 2;
        } else if (std::any_of(std::begin(kStop), std::end(kStop),
                               [&](const auto& s) { return (w & s.first) == s.second; })) {
            // str / strb / strh (immediate, unscaled) may store the same on every pass
            const std::uint32_t imm = w & 0xBFC00000;
            const std::uint32_t unscaled = w & 0xBFE00C00;
            const bool store = imm == 0xB9000000 || imm == 0x39000000 || imm == 0x79000000 || unscaled == 0xB8000000 ||
                               unscaled == 0x38000000 || unscaled == 0x78000000;
            if (!store) continue;
        }
        for (int i = 0; i < count; ++i) {
            if (succ[i] == head) return true;
        }
        for (int i = 0; i < count; ++i) todo.push_back(succ[i]);
    }
    return false;
}

std::optional<SpinRegion> Rewriter::FindSpinRegion(Addr head, const std::vector<Addr>& back_edges) {
    // A store whose value or address a pass changes is no part of a region: the paths through it are dropped and the
    // region is found again without them. When the paths through stores bring in a register that a pass changes, the
    // region is looked for once more with every store ending its path (spin_region).
    std::set<Addr> ban;
    SpinTry g;
    for (int i = 0; i < 8; ++i) {
        g = SpinRegion1(head, back_edges, ban, true);
        if (g.vary.empty()) break;
        ban.insert(g.vary.begin(), g.vary.end());
    }
    // The region without stores: the wider one only when it covers its back-edges
    SpinTry old = SpinRegion1(head, back_edges, {}, false);
    if (g.region && g.vary.empty() &&
        (!old.region || std::includes(g.region->valid.begin(), g.region->valid.end(), old.region->valid.begin(),
                                      old.region->valid.end()))) {
        return std::move(g.region);
    }
    return std::move(old.region);
}

Rewriter::SpinTry Rewriter::SpinRegion1(Addr head, const std::vector<Addr>& back_edges, const std::set<Addr>& ban,
                                        bool stores) {
    const Addr T = head;
    const Addr hi = *std::max_element(back_edges.begin(), back_edges.end());
    for (const auto& [s, e] : data_blocks_) {
        if (s < hi + 4 && T < e) return {};
    }
    if (!SpinQuick(T, hi)) return {};
    std::map<Addr, SpinNode> nodes;
    std::set<Addr> back, bad, inner;
    std::vector<Addr> todo{T};
    const auto reg = [](const Insn& x, int i) { return x.OpReg(i); };
    while (!todo.empty()) {
        const Addr a = todo.back();
        todo.pop_back();
        if (nodes.contains(a) || bad.contains(a)) continue;
        if (ban.contains(a)) {
            bad.insert(a);
            continue;
        }
        SpinNode n;
        bool made = false;
        // ld(a)xr wA,[xN]; add|sub wB,wA,#k; st(l)xr wS,wB,[xN]; cbnz wS,<the load> (spin_rmw)
        if (a + 12 <= hi && (Word(a) & 0x3FE00000) == 0x08400000) {
            const Insn* l[4];
            bool all = true;
            for (int i = 0; i < 4; ++i) all = (l[i] = SpinIns(a + 4 * i)) != nullptr && all;
            if (all && l[0]->IsAny("ldxr", "ldaxr") && l[1]->IsAny("add", "sub") && l[2]->IsAny("stxr", "stlxr") &&
                l[3]->Is("cbnz") && l[1]->op_count == 3 && l[1]->operands[2].type == ARM64_OP_IMM &&
                !absl::StrContains(l[1]->op_str, "lsl")) {
                const arm64_op_mem& base = l[0]->operands[1].mem;
                if (base.disp == 0 && base.index == 0 && l[2]->operands[2].mem.base == base.base &&
                    l[2]->operands[2].mem.disp == 0 && reg(*l[1], 1) == reg(*l[0], 0) &&
                    reg(*l[2], 1) == reg(*l[1], 0) && reg(*l[3], 0) == reg(*l[2], 0) && l[3]->operands[1].imm == a) {
                    n.type = SpinNode::kRmw;
                    n.base = SpinNorm(l[0]->Name(base.base)).value_or("");
                    n.k = static_cast<int>(l[1]->operands[2].imm) * (l[1]->Is("add") ? 1 : -1);
                    for (const Insn* x : l) n.ins.push_back(*x);
                    n.uses = {n.base};
                    for (int i = 0; i < 3; ++i) Add(&n.defs, SpinNorm(reg(*l[i], 0)));
                    n.has_use_def = true;
                    n.succ = {a + 16};
                    made = true;
                }
            }
        }
        if (!made) {
            // An exclusive spin lock sequence or a test loop entered at a (spin_lock):
            //   acq  ld(a)xr wA,[xN]; cbz wA,+16; clrex; b <the load>; st(l)xr wS,wK,[xN]; cbnz wS,<the load>
            //   rel  clrex; ld(a)xr wA,[xN]; cmp wA,#k; b.ne <the clrex>; st(l)xr wS,wzr,[xN]; cbnz wS,<the load>,
            //        entered at the load; or stlr wzr,[xN]
            //   tst  ldr|ldrb|ldrh wA,[xN{,#imm}]; cmp wA,#k; b.eq|b.ne <the load>
            const Insn* l[6];
            bool all = true;
            for (int i = 0; i < 6; ++i) all = (l[i] = SpinIns(a + 4 * i)) != nullptr && all;
            const auto lock = [&](SpinNode::Type type, const std::string& bn, const std::vector<const Insn*>& ins,
                                  int pre, int count, Names uses, Names defs) {
                n.type = type;
                n.base = bn;
                n.k = type == SpinNode::kAcq ? 1 : type == SpinNode::kRel ? -1 : 0;
                for (const Insn* x : ins) n.ins.push_back(*x);
                n.pre = pre;
                n.uses = std::move(uses);
                n.defs = std::move(defs);
                n.has_use_def = true;
                n.succ = {a + 4 * count};
                made = true;
            };
            if (l[0] != nullptr) {
                const std::string_view m0 = l[0]->mnemonic;
                if ((m0 == "ldxr" || m0 == "ldaxr") && all && reg(*l[0], 0).starts_with('w') && PlainMem(*l[0], 1)) {
                    const unsigned base = l[0]->operands[1].mem.base;
                    const std::string bn = SpinNorm(l[0]->Name(base)).value_or("");
                    if (l[1]->Is("cbz") && reg(*l[1], 0) == reg(*l[0], 0) && l[1]->operands[1].imm == a + 16 &&
                        l[2]->Is("clrex") && l[3]->Is("b") && l[3]->operands[0].imm == a &&
                        l[4]->IsAny("stxr", "stlxr") && PlainMem(*l[4], 2) && l[4]->operands[2].mem.base == base &&
                        reg(*l[4], 1) != "wzr" && reg(*l[4], 1) != "xzr" && l[5]->Is("cbnz") &&
                        reg(*l[5], 0) == reg(*l[4], 0) && l[5]->operands[1].imm == a) {
                        Names uses{bn}, defs;
                        Add(&uses, SpinNorm(reg(*l[4], 1)));
                        Add(&defs, SpinNorm(reg(*l[0], 0)));
                        Add(&defs, SpinNorm(reg(*l[4], 0)));
                        lock(SpinNode::kAcq, bn, {l[0], l[1], l[2], l[3], l[4], l[5]}, 0, 6, std::move(uses),
                             std::move(defs));
                    } else if (const Insn* p = SpinIns(a - 4);
                               p != nullptr && p->Is("clrex") && l[1]->Is("cmp") && reg(*l[1], 0) == reg(*l[0], 0) &&
                               l[1]->operands[1].type == ARM64_OP_IMM && l[2]->Is("b.ne") &&
                               l[2]->operands[0].imm == a - 4 && l[3]->IsAny("stxr", "stlxr") &&
                               reg(*l[3], 1) == "wzr" && PlainMem(*l[3], 2) && l[3]->operands[2].mem.base == base &&
                               l[4]->Is("cbnz") && reg(*l[4], 0) == reg(*l[3], 0) && l[4]->operands[1].imm == a) {
                        Names defs{"nzcv"};
                        Add(&defs, SpinNorm(reg(*l[0], 0)));
                        Add(&defs, SpinNorm(reg(*l[3], 0)));
                        lock(SpinNode::kRel, bn, {p, l[0], l[1], l[2], l[3], l[4]}, 1, 5, {bn}, std::move(defs));
                    }
                }
                if (!made && m0 == "stlr" && l[0]->op_count == 2 &&
                    (reg(*l[0], 0) == "wzr" || reg(*l[0], 0) == "xzr") && PlainMem(*l[0], 1)) {
                    const std::string bn = SpinNorm(l[0]->Name(l[0]->operands[1].mem.base)).value_or("");
                    lock(SpinNode::kRel, bn, {l[0]}, 0, 1, {bn}, {});
                }
                if (!made && (m0 == "ldr" || m0 == "ldrb" || m0 == "ldrh") && l[1] != nullptr && l[2] != nullptr &&
                    l[0]->op_count == 2 && reg(*l[0], 0).starts_with('w') && l[0]->operands[1].type == ARM64_OP_MEM &&
                    l[0]->operands[1].mem.index == 0 && !l[0]->writeback && l[1]->Is("cmp") &&
                    reg(*l[1], 0) == reg(*l[0], 0) && l[1]->operands[1].type == ARM64_OP_IMM &&
                    l[2]->IsAny("b.eq", "b.ne") && l[2]->operands[0].imm == a) {
                    const std::string bn = SpinNorm(l[0]->Name(l[0]->operands[1].mem.base)).value_or("");
                    Names defs{"nzcv"};
                    Add(&defs, SpinNorm(reg(*l[0], 0)));
                    // The region's own test loop is no node of it
                    if (a != T) lock(SpinNode::kTst, bn, {l[0], l[1], l[2]}, 0, 3, {bn}, std::move(defs));
                }
            }
        }
        if (!made && stores) {
            if (const std::optional<Store> st = SpinStore(SpinIns(a))) {
                n.type = SpinNode::kStore;
                n.store_value = st->value;
                n.store_base = st->base;
                n.ins.push_back(*SpinIns(a));
                n.uses = {st->base};
                Add(&n.uses, st->value);
                n.has_use_def = true;
                n.succ = {a + 4};
                made = true;
            }
        }
        if (!made) {
            const Insn* x = SpinIns(a);
            const std::optional<SpinNode::Type> k = SpinKind(x);
            if (!k) {
                bad.insert(a);
                continue;
            }
            n.type = *k;
            n.ins.push_back(*x);
            if (*k == SpinNode::kBranch) {
                n.succ = x->Is("b") ? std::vector<Addr>{x->Target()} : std::vector<Addr>{x->Target(), a + 4};
            } else {
                n.succ = {a + 4};
            }
        }
        bool touched = false;
        for (std::size_t i = 0; i < n.ins.size(); ++i) {
            touched = touched || spin_touched_.contains(a + 4 * (static_cast<Addr>(i) - n.pre));
        }
        if (touched) {
            bad.insert(a);
            continue;
        }
        if (n.type == SpinNode::kRmw) inner.insert({a + 4, a + 8, a + 12});
        if (n.type == SpinNode::kAcq || n.type == SpinNode::kRel || n.type == SpinNode::kTst) {
            for (std::size_t i = 0; i < n.ins.size(); ++i) {
                const Addr at = a + 4 * (static_cast<Addr>(i) - n.pre);
                if (at != a) inner.insert(at);
            }
        }
        for (const Addr s : n.succ) {
            if (s == T) {
                back.insert(a);
            } else if (T < s && s <= hi) {
                todo.push_back(s);
            }
        }
        nodes.emplace(a, std::move(n));
    }
    if (back.empty()) return {};
    for (const auto& [a, n] : nodes) {
        if (inner.contains(a)) return {};
        for (const Addr s : n.succ) {
            if (inner.contains(s)) return {};
        }
    }
    // The nodes from which one of ends is reached without passing T
    const auto prune = [&](const std::set<Addr>& ends) {
        std::set<Addr> keep = ends;
        for (bool more = true; more;) {
            more = false;
            for (const auto& [a, n] : nodes) {
                if (keep.contains(a)) continue;
                if (std::any_of(n.succ.begin(), n.succ.end(), [&](Addr s) { return s != T && keep.contains(s); })) {
                    keep.insert(a);
                    more = true;
                }
            }
        }
        return keep;
    };
    std::set<Addr> R = prune(back);
    for (const Addr a : R) {
        SpinNode& n = nodes.at(a);
        if (!n.has_use_def) {
            SpinRw(n.ins[0], &n.uses, &n.defs);
            n.has_use_def = true;
        }
    }
    // Every path from T to a back-edge: no inner cycle, and the exclusive adds on each address (a register between two
    // writes to it) sum to 0; a lock is taken once and released
    std::set<Addr> bad_back;
    int paths = 0;
    int steps = 0;
    using Version = std::map<std::string, int>;
    using Delta = std::map<std::tuple<bool, std::string, int>, int>;
    const std::function<void(Addr, const std::set<Addr>&, const Version&, const Delta&)> walk =
        [&](Addr a, const std::set<Addr>& onpath, const Version& ver_in, const Delta& delta_in) {
            if (++steps > kSpinSteps) throw SpinNo();
            const SpinNode& n = nodes.at(a);
            Delta delta = delta_in;
            if (n.type == SpinNode::kRmw || n.type == SpinNode::kAcq || n.type == SpinNode::kRel) {
                const auto base_version = ver_in.find(n.base);
                const std::tuple<bool, std::string, int> key(n.type != SpinNode::kRmw, n.base,
                                                             base_version == ver_in.end() ? 0 : base_version->second);
                const int sum = (delta[key] += n.k);
                // A release before its acquire or a second acquire: not a lock this pass takes once
                if (n.type != SpinNode::kRmw && !(0 <= sum && sum <= 1)) throw SpinNo();
            }
            Version ver = ver_in;
            for (const std::string& r : n.defs) ++ver[r];
            for (const Addr s : n.succ) {
                if (s == T) {
                    if (++paths > kSpinPaths) throw SpinNo();
                    if (std::any_of(delta.begin(), delta.end(), [](const auto& d) { return d.second != 0; }))
                        bad_back.insert(a);
                } else if (R.contains(s)) {
                    if (onpath.contains(s)) throw SpinNo();
                    std::set<Addr> next = onpath;
                    next.insert(s);
                    walk(s, next, ver, delta);
                }
            }
        };
    try {
        walk(T, {T}, {}, {});
    } catch (const SpinNo&) {
        return {};
    }
    std::set<Addr> valid;
    std::set_difference(back.begin(), back.end(), bad_back.begin(), bad_back.end(), std::inserter(valid, valid.end()));
    if (valid.empty()) return {};
    R = prune(valid);
    // No state carried from one pass to the next: nothing live at T is written in R
    std::map<Addr, Names> live;
    for (const Addr a : R) live[a];
    for (bool more = true; more;) {
        more = false;
        for (auto it = R.rbegin(); it != R.rend(); ++it) {
            const Addr a = *it;
            const SpinNode& n = nodes.at(a);
            Names out;
            for (const Addr s : n.succ) {
                if (s == T) {
                    if (valid.contains(a)) out.insert(live[T].begin(), live[T].end());
                } else if (R.contains(s)) {
                    out.insert(live[s].begin(), live[s].end());
                }
            }
            Names now = n.uses;
            for (const std::string& r : out) {
                if (!n.defs.contains(r)) now.insert(r);
            }
            if (now != live[a]) {
                live[a] = std::move(now);
                more = true;
            }
        }
    }
    Names defs;
    bool any_store = false;
    bool any_polled = false;
    int store_count = 0;
    for (const Addr a : R) {
        const SpinNode& n = nodes.at(a);
        defs.insert(n.defs.begin(), n.defs.end());
        if (n.type == SpinNode::kStore) {
            any_store = true;
            ++store_count;
        }
        any_polled = any_polled || n.type == SpinNode::kRmw || n.type == SpinNode::kAcq || n.type == SpinNode::kRel ||
                     n.type == SpinNode::kTst || n.ins[0].mnemonic.starts_with("ld");
    }
    if (any_store && !any_polled) return {};  // stores and nothing polled
    // A store's value and address must come from registers no pass writes
    SpinTry result;
    for (const Addr a : R) {
        const SpinNode& n = nodes.at(a);
        if (n.type != SpinNode::kStore) continue;
        if (std::any_of(n.uses.begin(), n.uses.end(), [&](const std::string& r) { return defs.contains(r); })) {
            result.vary.insert(a);
        }
    }
    if (!result.vary.empty()) return result;
    if (store_count > kSpinStores) return {};
    for (const std::string& r : live[T]) {
        if (defs.contains(r)) return {};
    }
    SpinRegion region;
    region.head = T;
    region.region.assign(R.begin(), R.end());
    region.nodes = std::move(nodes);
    region.valid = std::move(valid);
    result.region = std::move(region);
    return result;
}

void Rewriter::SpinEmit(const SpinRegion& g) {
    const Addr T = g.head;
    const std::vector<Addr>& R = g.region;
    UseArea(T);
    std::map<Addr, Addr> cpy;
    struct Fix {
        std::size_t at;      // in the patch area
        const Insn* branch;  // the branch to re-encode, or null for a plain b
        enum { kTo, kCopy, kOriginal, kOwn } how;
        Addr target;
    };
    std::vector<Fix> fixes;
    std::map<Addr, Addr> tramp;
    for (std::size_t i = 0; i < R.size(); ++i) {
        const Addr a = R[i];
        const SpinNode& n = g.nodes.at(a);
        cpy[a] = Here();
        const Insn& x = n.ins[0];
        switch (n.type) {
            case SpinNode::kRmw:
                for (int y = 0; y < 3; ++y) Emit(n.ins[static_cast<std::size_t>(y)].word);
                fixes.push_back({patch_->size(), &n.ins[3], Fix::kCopy, a});  // retry: the copy's exclusive load
                Emit(0);
                break;
            case SpinNode::kAcq:
            case SpinNode::kRel:  // copied as they are: their branches stay inside the sequence
                cpy[a] = Here() + 4 * n.pre;
                for (const Insn& y : n.ins) Emit(y.word);
                break;
            case SpinNode::kTst:
                // Its branch back goes to the original loop, which passes its own spin gate when it is one itself
                Emit(n.ins[0].word);
                Emit(n.ins[1].word);
                fixes.push_back({patch_->size(), &n.ins[2], Fix::kOriginal, a});
                Emit(0);
                break;
            case SpinNode::kPc: {
                const std::optional<int> rd = RegNum(SpinNorm(x.OpReg(0)).value_or(""));
                if (!rd) Fail(absl::StrFormat("spin wait: %s into %s at %s", x.mnemonic, x.OpReg(0), Hex(x.address)));
                const Addr target = x.operands[1].imm;
                Emit(Adrp(static_cast<unsigned>(*rd), Here(), target));
                if (x.Is("adr"))
                    Emit(AddI(static_cast<unsigned>(*rd), static_cast<unsigned>(*rd),
                              static_cast<unsigned>(target & 0xFFF)));
                break;
            }
            case SpinNode::kBranch:
                fixes.push_back({patch_->size(), &x, Fix::kOwn, 0});
                Emit(0);
                break;
            default:
                Emit(x.word);
        }
        if (!(n.type == SpinNode::kBranch && x.Is("b"))) {
            const Addr ft = a + 4 * (static_cast<Addr>(n.ins.size()) - n.pre);
            if (!(i + 1 < R.size() && R[i + 1] == ft)) {  // the next instruction is not the next one copied
                fixes.push_back({patch_->size(), nullptr, Fix::kTo, ft});
                Emit(0);
            }
        }
    }
    const Addr pause = Here();
    Emit(StpPre(29, 30, 31, -16));
    EmitGateCall();
    Emit(Blr(30));
    const Addr ret = Here();
    Emit(LdpPost(29, 30, 31, 16));
    gates_.push_back({ret, "spin", 0, T});
    Emit(B(Here(), cpy.at(T)));
    const auto out = [&](Addr t) {  // an exit taken by a conditional branch: through a b next to the copy
        const auto [it, added] = tramp.try_emplace(t, Here());
        if (added) Emit(B(Here(), t));
        return it->second;
    };
    for (const Fix& fix : fixes) {
        const Addr pc = patch_base_ + static_cast<Addr>(fix.at);
        std::uint32_t w = 0;
        if (fix.how == Fix::kTo) {
            const auto copied = cpy.find(fix.target);
            w = B(pc, copied != cpy.end() ? copied->second : fix.target);
        } else if (fix.how == Fix::kOriginal) {
            w = SpinRebranch(*fix.branch, pc, out(fix.target));
        } else if (fix.how == Fix::kCopy) {
            w = SpinRebranch(*fix.branch, pc, cpy.at(fix.target));
        } else {
            const Insn& x = *fix.branch;
            const Addr t = x.Target();
            Addr d;
            if (t == T && g.valid.contains(x.address)) {
                d = pause;
            } else if (cpy.contains(t) && t != T) {
                d = cpy.at(t);
            } else {
                d = !x.Is("b") ? out(t) : t;
            }
            w = SpinRebranch(x, pc, d);
        }
        std::memcpy(patch_->data() + fix.at, &w, 4);
    }
    for (const Addr b : g.valid) {  // the original back-edges enter the copy
        const Insn& x = g.nodes.at(b).ins[0];
        if (x.Is("b")) {
            PutBranch(b, cpy.at(T));
        } else {
            const Addr st = Here();
            Emit(SpinRebranch(x, Here(), cpy.at(T)));
            Emit(B(Here(), b + 4));
            PutBranch(b, st);
        }
    }
    Stat("spin");
}

void Rewriter::SpinWaits() {
    for (const auto& [a, site] : kinds_) spin_touched_.insert(a);
    spin_touched_.insert(done_.begin(), done_.end());
    for (const ObservationGate& gate : obs_) spin_touched_.insert(gate.offset);
    for (const Cut& cut : cuts_) {
        for (Addr o = cut.offset; o < cut.offset + static_cast<Addr>(cut.original.size() / 2); o += 4)
            spin_touched_.insert(o);
    }
    // All found on the unchanged image first: emitting patches back-edges
    for (const auto& [head, back_edges] : spin_cand_) {
        if (std::optional<SpinRegion> region = FindSpinRegion(head, back_edges))
            spin_regions_.push_back(std::move(*region));
        if (spin_dec_.size() > 100000) spin_dec_.clear();
    }
    for (const SpinRegion& g : spin_regions_) SpinEmit(g);
    if (options_.list_spin_waits) {
        for (const SpinRegion& g : spin_regions_) {
            std::size_t count = 0;
            for (const Addr a : g.region) count += g.nodes.at(a).ins.size();
            Log(absl::StrFormat("%s spin wait %s (%d instructions), back-edges %s", name_, Hex(g.head), count,
                                absl::StrJoin(g.valid, " ", [](std::string* o, Addr a) { o->append(Hex(a)); })));
        }
    }
}

}  // namespace nxmac::rewriter::internal
