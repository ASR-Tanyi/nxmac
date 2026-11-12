// AArch64 instructions as Capstone 5 decodes them (internal to nxmac::rewriter): the mnemonic and operand text, the
// operands, and the registers read and written, exactly what nxnative's rewriter gets from Capstone's Python binding.
// One Disassembler serves one thread.
#pragma once

#include <capstone/capstone.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/inlined_vector.h"

namespace nxmac::rewriter::internal {

class Disassembler;

struct Insn {
    std::int64_t address = 0;
    std::uint32_t word = 0;
    std::string mnemonic;
    std::string op_str;
    // The details (empty for an instruction decoded without them)
    std::uint8_t op_count = 0;
    std::array<cs_arm64_op, 8> operands{};
    bool writeback = false;
    bool regs_ok = false;  // cs_regs_access answered: the two lists below are its
    absl::InlinedVector<std::uint16_t, 6> regs_read;
    absl::InlinedVector<std::uint16_t, 6> regs_write;
    const Disassembler* md = nullptr;

    bool Is(std::string_view m) const { return mnemonic == m; }
    template <typename... T>
    bool IsAny(T... m) const {
        return (Is(m) || ...);
    }
    // The last operand's immediate: a direct branch's target
    std::int64_t Target() const { return operands[op_count - 1].imm; }
    // Capstone's name of a register ID: x0, w18, sp, wsp, xzr, and fp / lr for x29 / x30; empty when it has none
    std::string_view Name(unsigned reg) const;
    // The name of operand i's register
    std::string_view OpReg(int i) const { return Name(operands[static_cast<std::size_t>(i)].reg); }
};

class Disassembler {
public:
    Disassembler();
    ~Disassembler();
    Disassembler(const Disassembler&) = delete;
    Disassembler& operator=(const Disassembler&) = delete;

    // The instructions of code at address, with details, up to the first word that does not decode (as cs_disasm)
    std::vector<Insn> Disasm(std::span<const std::uint8_t> code, std::int64_t address) const;
    // One word with details, or nullopt when it does not decode
    std::optional<Insn> One(std::uint32_t word, std::int64_t address) const;
    // One word without details: whether it decodes, and its operand text when asked for
    bool Lite(std::uint32_t word, std::int64_t address, std::string* op_str = nullptr) const;

    std::string_view RegName(unsigned reg) const;

private:
    void Fill(const cs_insn& raw, Insn* out) const;

    csh detailed_ = 0;
    csh lite_ = 0;
    cs_insn* scratch_ = nullptr;
    cs_insn* scratch_lite_ = nullptr;
};

// ---- what nxnative's rewriter derives from an instruction ----

// Bit n set for each general register xN / wN the instruction writes by Capstone's list (fp is x29, lr x30). Capstone
// lists a compare's first operand as written too: callers that want real writes leave compares out (WritesReg).
std::uint32_t WrittenMask(const Insn& p);
// Whether p changes general register n: a write of wN clears the upper half of xN too, and a compare only sets flags
bool WritesReg(const Insn& p, int n);
inline bool IsCompare(const Insn& p) { return p.IsAny("cmp", "cmn", "tst"); }
// b.eq, b.hi, ...: a conditional branch on the flags
inline bool IsBCond(const Insn& p) { return p.mnemonic.size() > 2 && p.mnemonic[0] == 'b' && p.mnemonic[1] == '.'; }
// cbz / cbnz / tbz / tbnz
inline bool IsCbTb(const Insn& p) { return p.IsAny("cbz", "cbnz", "tbz", "tbnz"); }

}  // namespace nxmac::rewriter::internal
