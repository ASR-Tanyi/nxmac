#include "nxmac/fs/ticket.h"

#include <algorithm>

#include "absl/strings/str_format.h"
#include "bytes.h"

namespace nxmac::fs {

absl::StatusOr<Ticket> ParseTicket(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4) return absl::InvalidArgumentError("too small for a ticket");
    // The signature's size and its padding, by signature type (RSA-4096, RSA-2048, ECDSA, HMAC; SHA-1 or SHA-256)
    std::size_t signature = 0;
    switch (internal::LoadLe<std::uint32_t>(bytes, 0)) {
        case 0x010000:
        case 0x010003:
            signature = 0x200 + 0x3c;
            break;
        case 0x010001:
        case 0x010004:
            signature = 0x100 + 0x3c;
            break;
        case 0x010002:
        case 0x010005:
            signature = 0x3c + 0x40;
            break;
        case 0x010006:
            signature = 0x14 + 0x28;
            break;
        default:
            return absl::InvalidArgumentError(
                absl::StrFormat("not a ticket: unknown signature type %#x", internal::LoadLe<std::uint32_t>(bytes, 0)));
    }
    const std::size_t body = 4 + signature;
    constexpr std::size_t kBodySize = 0x180;
    if (bytes.size() < body + kBodySize) return absl::InvalidArgumentError("ticket ends inside its body");
    Ticket ticket;
    std::copy_n(bytes.begin() + body + 0x40, ticket.title_key.size(), ticket.title_key.begin());
    ticket.common = bytes[body + 0x141] == 0;
    ticket.key_generation = bytes[body + 0x145];
    std::copy_n(bytes.begin() + body + 0x160, ticket.rights_id.size(), ticket.rights_id.begin());
    return ticket;
}

}  // namespace nxmac::fs
