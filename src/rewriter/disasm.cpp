#include "disasm.h"

#include <cstring>

#include "error.h"
#include "text.h"

namespace nxmac::rewriter::internal {

std::string_view Insn::Name(unsigned reg) const { return md->RegName(reg); }

Disassembler::Disassembler() {
    if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &detailed_) != CS_ERR_OK ||
        cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &lite_) != CS_ERR_OK) {
        Fail("Capstone does not open for AArch64");
    }
    cs_option(detailed_, CS_OPT_DETAIL, CS_OPT_ON);
    scratch_ = cs_malloc(detailed_);
    scratch_lite_ = cs_malloc(lite_);
}

Disassembler::~Disassembler() {
    if (scratch_ != nullptr) cs_free(scratch_, 1);
    if (scratch_lite_ != nullptr) cs_free(scratch_lite_, 1);
    if (detailed_ != 0) cs_close(&detailed_);
    if (lite_ != 0) cs_close(&lite_);
}

std::string_view Disassembler::RegName(unsigned reg) const {
    const char* name = cs_reg_name(detailed_, reg);
    return name == nullptr ? std::string_view() : std::string_view(name);
}

void Disassembler::Fill(const cs_insn& raw, Insn* out) const {
    out->address = static_cast<std::int64_t>(raw.address);
    std::memcpy(&out->word, raw.bytes, 4);
    out->mnemonic = raw.mnemonic;
    out->op_str = raw.op_str;
    out->md = this;
    const cs_arm64& arm64 = raw.detail->arm64;
    out->op_count = arm64.op_count;
    for (std::size_t i = 0; i < arm64.op_count; ++i) out->operands[i] = arm64.operands[i];
    out->writeback = arm64.writeback;
    cs_regs read;
    cs_regs write;
    std::uint8_t read_count = 0;
    std::uint8_t write_count = 0;
    out->regs_ok = cs_regs_access(detailed_, &raw, read, &read_count, write, &write_count) == CS_ERR_OK;
    if (out->regs_ok) {
        out->regs_read.assign(read, read + read_count);
        out->regs_write.assign(write, write + write_count);
    }
}

std::vector<Insn> Disassembler::Disasm(std::span<const std::uint8_t> code, std::int64_t address) const {
    std::vector<Insn> out;
    out.reserve(code.size() / 4);
    const std::uint8_t* at = code.data();
    std::size_t left = code.size();
    auto pc = static_cast<std::uint64_t>(address);
    while (cs_disasm_iter(detailed_, &at, &left, &pc, scratch_)) Fill(*scratch_, &out.emplace_back());
    return out;
}

std::optional<Insn> Disassembler::One(std::uint32_t word, std::int64_t address) const {
    std::uint8_t bytes[4];
    std::memcpy(bytes, &word, 4);
    const std::uint8_t* at = bytes;
    std::size_t left = 4;
    auto pc = static_cast<std::uint64_t>(address);
    if (!cs_disasm_iter(detailed_, &at, &left, &pc, scratch_)) return std::nullopt;
    std::optional<Insn> out(std::in_place);
    Fill(*scratch_, &*out);
    return out;
}

bool Disassembler::Lite(std::uint32_t word, std::int64_t address, std::string* op_str) const {
    std::uint8_t bytes[4];
    std::memcpy(bytes, &word, 4);
    const std::uint8_t* at = bytes;
    std::size_t left = 4;
    auto pc = static_cast<std::uint64_t>(address);
    if (!cs_disasm_iter(lite_, &at, &left, &pc, scratch_lite_)) return false;
    if (op_str != nullptr) *op_str = scratch_lite_->op_str;
    return true;
}

std::uint32_t WrittenMask(const Insn& p) {
    std::uint32_t mask = 0;
    for (const std::uint16_t reg : p.regs_write) {
        std::string_view name = p.Name(reg);
        if (name == "lr") name = "x30";
        if (name == "fp") name = "x29";
        if (const std::optional<int> n = RegNum(name); n && *n < 32) mask |= 1U << *n;
    }
    return mask;
}

bool WritesReg(const Insn& p, int n) { return !IsCompare(p) && (WrittenMask(p) >> n & 1) != 0; }

}  // namespace nxmac::rewriter::internal
