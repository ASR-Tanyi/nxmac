// Control data: control.nacp in a Control NCA's RomFS (the title's names, publishers and display version), and its
// icons, /icon_<language>.dat, one JPEG per language.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace nxmac::fs {

struct ControlData {
    std::string name;             // the first language's that has one (American English first)
    std::string publisher;        // of the same language
    std::string display_version;  // "1.9.0"
};

// control.nacp: its names are 16 languages of 0x300 bytes (a name of 0x200, a publisher of 0x100), or, from newer SDKs,
// a size and raw deflate data that inflates to 32 such languages. InvalidArgument when it is shorter than a NACP.
absl::StatusOr<ControlData> ParseNacp(std::span<const std::uint8_t> nacp);

// The languages of the icons, in the order nxnative's nx_extract takes them: English first, then the NACP's order
inline constexpr std::array<std::string_view, 16> kIconLanguages = {
    "AmericanEnglish", "BritishEnglish",       "Japanese",          "French",
    "German",          "LatinAmericanSpanish", "Spanish",           "Italian",
    "Dutch",           "CanadianFrench",       "Portuguese",        "Russian",
    "Korean",          "TraditionalChinese",   "SimplifiedChinese", "BrazilianPortuguese"};

}  // namespace nxmac::fs
