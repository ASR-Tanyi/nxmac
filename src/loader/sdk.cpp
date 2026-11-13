#include "nxmac/loader/sdk.h"

#include <algorithm>
#include <string_view>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"

namespace nxmac::loader {
namespace {

bool IsAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

SdkVersion FindSdkVersion(std::span<const std::uint8_t> data) {
    const std::string_view s(reinterpret_cast<const char*>(data.data()), data.size());
    constexpr std::string_view kMarker = "_nnSdk-";
    constexpr std::string_view kVendor = "NintendoSDK";
    SdkVersion version;
    for (std::size_t p = s.find(kMarker); p != std::string_view::npos; p = s.find(kMarker, p + kMarker.size())) {
        if (p < kVendor.size() || !absl::EqualsIgnoreCase(s.substr(p - kVendor.size(), kVendor.size()), kVendor))
            continue;
        const std::size_t start = p + kMarker.size();
        std::size_t v = start;
        while (v < s.size() && IsAlpha(s[v])) ++v;  // a variant name ("Generic") and its '_'
        if (v > start && v < s.size() && s[v] == '_') ++v;
        if (v >= s.size() || !IsDigit(s[v])) continue;
        int major = 0;
        for (; v < s.size() && IsDigit(s[v]) && major < 100000; ++v) major = major * 10 + (s[v] - '0');  // atoi
        // %.20s: up to 20 characters, or to the string's end
        const std::string_view rest = s.substr(start, 20);
        version.text = std::string(rest.substr(0, std::min(rest.find('\0'), rest.size())));
        version.major = major;
        break;
    }
    return version;
}

bool IsOunce(std::span<const std::uint8_t> data) {
    const std::string_view s(reinterpret_cast<const char*>(data.data()), data.size());
    return absl::StrContains(s, "spec: Ounce");
}

}  // namespace nxmac::loader
