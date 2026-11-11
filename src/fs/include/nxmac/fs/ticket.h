// Tickets (.tik in an NSP): the title key of the content whose NCAs carry a rights ID.
#pragma once

#include <cstdint>
#include <span>

#include "absl/status/statusor.h"
#include "nxmac/fs/crypto.h"
#include "nxmac/fs/keys.h"

namespace nxmac::fs {

struct Ticket {
    RightsId rights_id{};
    // A common ticket's title key, still encrypted with the titlekek of key_generation; a personalized ticket's is
    // encrypted for one console and cannot be used here
    Aes128Key title_key{};
    bool common = false;
    std::uint8_t key_generation = 0;  // the master key revision
};

// InvalidArgument when bytes are not a ticket of a known signature type
absl::StatusOr<Ticket> ParseTicket(std::span<const std::uint8_t> bytes);

}  // namespace nxmac::fs
