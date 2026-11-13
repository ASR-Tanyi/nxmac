// A made-up module image for the loader's tests: MOD0 at 0x100, the dynamic section at 0x200, the symbols at 0x400,
// then the strings and the relocation tables; [kData, kData + kDataSize) is free for the relocations to write.
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nxmac/loader/relocate.h"
#include "nxmac/loader/symbols.h"

namespace nxmac::loader::testing {

inline constexpr std::uint64_t kData = 0x8000;
inline constexpr std::uint64_t kDataSize = 0x2000;
inline constexpr std::uint8_t kGlobalFunc = 0x12;  // st_info: STB_GLOBAL, STT_FUNC
inline constexpr std::uint8_t kLocalFunc = 0x02;
inline constexpr std::uint8_t kWeakFunc = 0x22;
inline constexpr std::uint8_t kHidden = 2;  // st_other: STV_HIDDEN

class ElfBuilder {
public:
    // A symbol; its index (the null symbol is 0)
    std::uint32_t Sym(const std::string& name, std::uint16_t shndx, std::uint64_t value,
                      std::uint8_t info = kGlobalFunc, std::uint8_t other = 0) {
        syms_.push_back({name, shndx, value, info, other});
        return static_cast<std::uint32_t>(syms_.size());
    }
    void Rela(std::uint64_t off, std::uint32_t sym, std::uint32_t type, std::int64_t addend = 0) {
        rela_.push_back({off, (std::uint64_t{sym} << 32) | type, addend});
    }
    void JmpRel(std::uint64_t off, std::uint32_t sym) {
        jmprel_.push_back({off, (std::uint64_t{sym} << 32) | kRAarch64JumpSlot, 0});
    }
    void Relr(std::vector<std::uint64_t> entries) { relr_ = std::move(entries); }
    // A word the image holds at `off` before relocation (RELR adds to it)
    void Word(std::uint64_t off, std::uint64_t v) { words_.push_back({off, v}); }

    std::vector<std::uint8_t> Build() const {
        std::vector<std::uint8_t> img(kData + kDataSize);
        Put<std::uint32_t>(img, 4, 0x100);
        std::memcpy(img.data() + 0x100, "MOD0", 4);
        Put<std::int32_t>(img, 0x104, 0x100);
        // The symbols, then their names
        const std::uint64_t symtab = 0x400;
        const std::uint64_t strtab = symtab + (syms_.size() + 1) * sizeof(Elf64Sym);
        std::uint64_t str = 1;
        for (std::size_t i = 0; i < syms_.size(); ++i) {
            const Elf64Sym s = {
                static_cast<std::uint32_t>(str), syms_[i].info, syms_[i].other, syms_[i].shndx, syms_[i].value, 0};
            Put(img, symtab + (i + 1) * sizeof(Elf64Sym), s);
            std::memcpy(img.data() + strtab + str, syms_[i].name.c_str(), syms_[i].name.size() + 1);
            str += syms_[i].name.size() + 1;
        }
        // The tables, each 8-aligned after the strings
        std::uint64_t at = (strtab + str + 7) & ~std::uint64_t{7};
        const auto table = [&](const void* data, std::size_t bytes) {
            const std::uint64_t off = at;
            if (bytes != 0) std::memcpy(img.data() + off, data, bytes);
            at += (bytes + 7) & ~std::size_t{7};
            return off;
        };
        const std::uint64_t rela = table(rela_.data(), rela_.size() * sizeof(Elf64Rela));
        const std::uint64_t jmprel = table(jmprel_.data(), jmprel_.size() * sizeof(Elf64Rela));
        const std::uint64_t relr = table(relr_.data(), relr_.size() * 8);
        if (at > kData) throw std::runtime_error("ElfBuilder: the tables reach the data");
        std::vector<Elf64Dyn> dyn = {{kDtSymtab, symtab}, {kDtStrtab, strtab}};
        if (!rela_.empty()) dyn.insert(dyn.end(), {{kDtRela, rela}, {kDtRelaSz, rela_.size() * sizeof(Elf64Rela)}});
        if (!jmprel_.empty()) {
            dyn.insert(dyn.end(), {{kDtJmpRel, jmprel}, {kDtPltRelSz, jmprel_.size() * sizeof(Elf64Rela)}});
        }
        if (!relr_.empty()) dyn.insert(dyn.end(), {{kDtRelr, relr}, {kDtRelrSz, relr_.size() * 8}});
        dyn.push_back({0, 0});
        std::memcpy(img.data() + 0x200, dyn.data(), dyn.size() * sizeof(Elf64Dyn));
        for (const auto& [off, v] : words_) Put(img, off, v);
        return img;
    }

private:
    template <typename T>
    static void Put(std::vector<std::uint8_t>& img, std::uint64_t off, const T& v) {
        std::memcpy(img.data() + off, &v, sizeof v);
    }

    struct S {
        std::string name;
        std::uint16_t shndx;
        std::uint64_t value;
        std::uint8_t info;
        std::uint8_t other;
    };
    std::vector<S> syms_;
    std::vector<Elf64Rela> rela_;
    std::vector<Elf64Rela> jmprel_;
    std::vector<std::uint64_t> relr_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> words_;
};

}  // namespace nxmac::loader::testing
