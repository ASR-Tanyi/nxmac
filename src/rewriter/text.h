// Text helpers of the rewriter (internal to nxmac::rewriter): numbers as nxnative's tables write them, and the matching
// of Capstone's operand text. nxnative's rewriter matches that text with regular expressions; the same patterns are
// written here as literal text with placeholders, so each rule reads as its Python original does.
#pragma once

#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nxmac::rewriter::internal {

// As Python's f'{value:#x}': 0x0, 0x1a, -0x2000000. Hex takes any integer type: std::size_t and std::uint64_t are
// one type on Linux and two on macOS, where overloads on the fixed-width types would make Hex(size) ambiguous.
std::string HexOf(std::int64_t value);
template <std::integral T>
std::string Hex(T value) {
    return HexOf(static_cast<std::int64_t>(value));
}
// As f'{value:x}'
std::string HexBare(std::int64_t value);
// As f'{value:08x}'
std::string Hex8(std::uint32_t value);
// Bytes as lower-case hex digits, and back (nullopt for anything but whole bytes of hex digits)
std::string BytesHex(const std::uint8_t* data, std::size_t size);
std::optional<std::vector<std::uint8_t>> HexBytes(std::string_view hex);

// Whether the whole of text matches pattern, whose characters stand for themselves except:
//   %x  a 64-bit register name, x and digits (as the regular expression x\d+)
//   %w  a 32-bit register name, w and digits (w\d+)
//   %r  either ([wx]\d+)
//   %n  digits (\d+)
//   %i  a number as Capstone prints an immediate: 0x and hex digits, or digits (0x[0-9a-f]+|\d+)
// Each placeholder's text goes to the next of captures (when there is one left and it is not null).
bool Scan(std::string_view text, std::string_view pattern, std::initializer_list<std::string_view*> captures = {});

// As Python's int(text, 0) for what Capstone prints: 0x1f, 12, -0x10
std::int64_t ParseInt(std::string_view text);

// The number of a general register's name (x17, w3; not sp, zr, fp or lr), as the regular expression [xw](\d+)
std::optional<int> RegNum(std::string_view name);

// Whether the operand text names x18 or w18 (\b[xw]18\b)
bool Has18(std::string_view op_str);
// The numbers of the general registers the operand text names (\b[xw](\d+)\b)
std::vector<int> RegsNamed(std::string_view op_str);
// The operand text with x18 / w18 replaced by x<s> / w<s>
std::string Subst18(std::string_view op_str, int s);

}  // namespace nxmac::rewriter::internal
