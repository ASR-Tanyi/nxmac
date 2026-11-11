// Switch title IDs: the 64-bit program IDs that name a game, its update and its add-on content (DLC), written as
// nxnative's profiles (dm/games/<id>) and NSP file names write them: 16 upper-case hex digits, e.g. 01007EF00011E000.
#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/statusor.h"

namespace nxmac::core {

class TitleId {
public:
    constexpr TitleId() = default;
    constexpr explicit TitleId(std::uint64_t value) : value_(value) {}

    // 16 hex digits in either case; anything else (a 0x prefix, a sign, spaces, a wrong length) is InvalidArgument.
    static absl::StatusOr<TitleId> Parse(std::string_view text);

    constexpr std::uint64_t value() const { return value_; }

    // The update of a base game (Horizon's patch ID): 01007EF00011E000 -> 01007EF00011E800.
    constexpr TitleId Update() const { return TitleId(value_ + 0x800); }

    // The add-on content with this index: base + 0x1000 + index, so index 1 of 01007EF00011E000 is 01007EF00011F001.
    constexpr TitleId AddOnContent(std::uint32_t index) const { return TitleId(value_ + 0x1000 + index); }

    // 16 upper-case hex digits, as Parse reads them.
    std::string ToString() const;

    friend constexpr bool operator==(TitleId, TitleId) = default;
    friend constexpr auto operator<=>(TitleId, TitleId) = default;

    template <typename H>
    friend H AbslHashValue(H h, TitleId id) {
        return H::combine(std::move(h), id.value_);
    }

    template <typename Sink>
    friend void AbslStringify(Sink& sink, TitleId id) {
        sink.Append(id.ToString());
    }

private:
    std::uint64_t value_ = 0;
};

}  // namespace nxmac::core
