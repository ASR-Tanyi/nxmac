#include "lz4.h"

#include <algorithm>
#include <cstring>

namespace nxmac::rewriter::internal {

std::ptrdiff_t Lz4DecompressBlock(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
    std::size_t ip = 0;
    std::size_t op = 0;
    // A length of 15 (literals) or 15 + 4 (match) goes on in bytes of 255, ended by a smaller one
    const auto more = [&](std::size_t* length) {
        std::uint8_t b = 255;
        while (b == 255) {
            if (ip >= in.size()) return false;
            b = in[ip++];
            *length += b;
        }
        return true;
    };
    while (ip < in.size()) {
        const std::uint8_t token = in[ip++];
        std::size_t literals = token >> 4;
        if (literals == 15 && !more(&literals)) return -1;
        if (literals > in.size() - ip || literals > out.size() - op) return -1;
        if (literals != 0) std::memcpy(out.data() + op, in.data() + ip, literals);
        ip += literals;
        op += literals;
        if (ip == in.size()) break;  // the last sequence is literals alone
        if (in.size() - ip < 2) return -1;
        const std::size_t offset = static_cast<std::size_t>(in[ip]) | static_cast<std::size_t>(in[ip + 1]) << 8;
        ip += 2;
        if (offset == 0 || offset > op) return -1;
        std::size_t match = token & 15;
        if (match == 15 && !more(&match)) return -1;
        match += 4;
        if (match > out.size() - op) return -1;
        // A match may overlap what it writes (offset < match repeats the last offset bytes): each copy takes what is
        // already whole, which doubles with every round
        std::uint8_t* to = out.data() + op;
        const std::uint8_t* from = to - offset;
        for (std::size_t left = match, have = offset; left != 0;) {
            const std::size_t n = std::min(left, have);
            std::memcpy(to, from, n);
            to += n;
            left -= n;
            have += n;
        }
        op += match;
    }
    return static_cast<std::ptrdiff_t>(op);
}

}  // namespace nxmac::rewriter::internal
