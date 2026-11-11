#include "nxmac/core/title_id.h"

#include <charconv>
#include <cstddef>
#include <system_error>

#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace nxmac::core {

absl::StatusOr<TitleId> TitleId::Parse(std::string_view text) {
    constexpr std::size_t kDigits = 16;
    if (text.size() == kDigits) {
        std::uint64_t value = 0;
        const char* const end = text.data() + text.size();
        const auto [ptr, ec] = std::from_chars(text.data(), end, value, 16);
        if (ec == std::errc() && ptr == end) return TitleId(value);
    }
    return absl::InvalidArgumentError(
        absl::StrCat("a title ID is 16 hex digits, not \"", absl::CHexEscape(text), "\""));
}

std::string TitleId::ToString() const { return absl::StrFormat("%016X", value_); }

}  // namespace nxmac::core
