// AArch64 instruction encoders of the rewriter and of the per-build stubs (nxnative's dm/tools/nso_asm.py), internal
// to nxmac::rewriter. Each returns the instruction word; a branch that does not reach its target is a RewriteError.
#pragma once

#include <cstdint>

#include "error.h"

namespace nxmac::rewriter::internal::a64 {

using Word = std::uint32_t;
using Addr = std::int64_t;  // an offset from the module's base: the patch area may lie before it

inline constexpr Word kNop = 0xD503201F;

inline Word B(Addr from, Addr to) {
    const Addr off = to - from;
    if (!(-(Addr{1} << 27) <= off && off < (Addr{1} << 27) && off % 4 == 0)) FailBranch("b", from, to);
    return 0x14000000 | static_cast<Word>((off >> 2) & 0x3FFFFFF);
}
inline Word Bl(Addr from, Addr to) { return B(from, to) | 0x80000000; }
inline Word CbnzW(unsigned t, Addr from, Addr to) {
    const Addr off = to - from;
    if (!(-(Addr{1} << 20) <= off && off < (Addr{1} << 20) && off % 4 == 0)) FailBranch("cbnz", from, to);
    return 0x35000000 | static_cast<Word>(((off >> 2) & 0x7FFFF) << 5) | t;
}
// Python's off // 8 floors; so does an arithmetic shift
inline Word Pair(Word op, unsigned t, unsigned t2, unsigned n, int off) {
    return op | static_cast<Word>((off >> 3) & 0x7F) << 15 | t2 << 10 | n << 5 | t;
}
inline Word Stp(unsigned t, unsigned t2, unsigned n, int off) { return Pair(0xA9000000, t, t2, n, off); }
inline Word Ldp(unsigned t, unsigned t2, unsigned n, int off) { return Pair(0xA9400000, t, t2, n, off); }
inline Word StpPre(unsigned t, unsigned t2, unsigned n, int off) { return Pair(0xA9800000, t, t2, n, off); }
inline Word LdpPost(unsigned t, unsigned t2, unsigned n, int off) { return Pair(0xA8C00000, t, t2, n, off); }
inline Word Stur(unsigned t, unsigned n, int off) {  // stur xt,[xn,#off] (off may be negative)
    return 0xF8000000 | static_cast<Word>(off & 0x1FF) << 12 | n << 5 | t;
}
inline Word Ldur(unsigned t, unsigned n, int off) {
    return 0xF8400000 | static_cast<Word>(off & 0x1FF) << 12 | n << 5 | t;
}
inline Word LdrX(unsigned t, unsigned n, unsigned imm12 = 0) { return 0xF9400000 | imm12 << 10 | n << 5 | t; }
inline Word StrX(unsigned t, unsigned n, unsigned imm12 = 0) { return 0xF9000000 | imm12 << 10 | n << 5 | t; }
inline Word MrsTpidrro(unsigned t) { return 0xD53BD060 | t; }
inline Word Adrp(unsigned d, Addr pc, Addr target) {
    const Addr imm = ((target & ~Addr{0xFFF}) - (pc & ~Addr{0xFFF})) >> 12;
    if (!(-(Addr{1} << 20) <= imm && imm < (Addr{1} << 20))) FailBranch("adrp", pc, target);
    return 0x90000000 | static_cast<Word>(imm & 3) << 29 | static_cast<Word>((imm >> 2) & 0x7FFFF) << 5 | d;
}
inline Word AddI(unsigned d, unsigned n, unsigned imm12, unsigned sh = 0) {
    return 0x91000000 | sh << 22 | imm12 << 10 | n << 5 | d;
}
inline Word AddR(unsigned d, unsigned n, unsigned m) { return 0x8B000000 | m << 16 | n << 5 | d; }
inline Word SubI(unsigned d, unsigned n, unsigned imm12, unsigned sh = 0) {
    return 0xD1000000 | sh << 22 | imm12 << 10 | n << 5 | d;
}
inline Word Br(unsigned n) { return 0xD61F0000 | n << 5; }
inline Word CmpR(unsigned n, unsigned m) { return 0xEB00001F | m << 16 | n << 5; }  // cmp xn, xm
inline Word Beq(Addr from, Addr to) {
    const Addr off = to - from;
    if (!(-(Addr{1} << 20) <= off && off < (Addr{1} << 20) && off % 4 == 0)) FailBranch("b.eq", from, to);
    return 0x54000000 | static_cast<Word>(((off >> 2) & 0x7FFFF) << 5);
}
inline Word Brk(unsigned imm) { return 0xD4200000 | (imm & 0xFFFF) << 5; }
inline Word Blr(unsigned n) { return 0xD63F0000 | n << 5; }

}  // namespace nxmac::rewriter::internal::a64
