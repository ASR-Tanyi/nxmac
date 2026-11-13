// Python's str, as the shader build's modules use it (internal to nxmac::dmsh). nxnative's dmsh_*.py rewrite Metal
// source with str methods and slices between regular-expression matches; their ports keep each step, so these do what
// the methods do on the Metal source, which is ASCII (an index is a byte's, as it is a character's there):
//   - Find / RFind give -1 when absent, as str.find does, and take Python's start and end (clamped, a negative one
//     counting from the end); Index throws ValueError as str.index does;
//   - Slice clamps and counts negative bounds from the end;
//   - Strip strips what str.strip() strips of ASCII (space, \t \n \v \f \r and \x1c-\x1f);
//   - Split(s, sep) is s.split(sep); SplitWs is s.split(); SplitLines is s.splitlines().
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/match.h"

namespace nxmac::dmsh::py {

// Python's ValueError: a shader the rewrite does not understand, which the build reports and leaves out
class ValueError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A stop of the whole build (sys.exit('[RED] ...') in the Python, or an exception it does not catch): the message
class Fatal : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Int = std::int64_t;

// s[a:b]
std::string_view Slice(std::string_view s, Int a, Int b);
inline std::string_view From(std::string_view s, Int a) { return Slice(s, a, static_cast<Int>(s.size())); }
inline std::string_view To(std::string_view s, Int b) { return Slice(s, 0, b); }

// s.find(sub, start, end), s.rfind(sub, start, end): -1 when absent
Int Find(std::string_view s, std::string_view sub, Int start = 0, Int end = INT64_MAX);
Int RFind(std::string_view s, std::string_view sub, Int start = 0, Int end = INT64_MAX);
// s.index(sub, start): ValueError("substring not found") when absent
Int Index(std::string_view s, std::string_view sub, Int start = 0);
inline bool Contains(std::string_view s, std::string_view sub) { return absl::StrContains(s, sub); }
// s.count(sub): non-overlapping
Int Count(std::string_view s, std::string_view sub);

// s.replace(old, new, count): count < 0 replaces all
std::string Replace(std::string_view s, std::string_view old, std::string_view repl, Int count = -1);

bool IsSpace(char c);
std::string_view Strip(std::string_view s);
std::string_view LStrip(std::string_view s);
std::string_view RStrip(std::string_view s);
// s.strip(chars) and the like with the characters given
std::string_view RStripChars(std::string_view s, std::string_view chars);

std::vector<std::string> Split(std::string_view s, std::string_view sep, Int maxsplit = -1);
std::vector<std::string> SplitWs(std::string_view s);
std::vector<std::string> SplitLines(std::string_view s);
std::string Join(const std::vector<std::string>& parts, std::string_view sep);

inline bool StartsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
inline bool EndsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}
std::string_view RemoveSuffix(std::string_view s, std::string_view suffix);

// str.isalnum() / isdigit() / isalpha() of one ASCII character, and of a whole string (false when empty)
inline bool IsAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool IsDigitChar(char c) { return c >= '0' && c <= '9'; }
bool IsDigit(std::string_view s);

// int(s, base): ValueError when s is not one; base 0 as Python's (0x, 0o, 0b prefixes)
Int ToInt(std::string_view s, int base = 10);
// float(s)
double ToFloat(std::string_view s);

// f'{v:#x}'
std::string Hex(Int v);

// sorted(set(...)) and friends
template <typename T>
std::vector<T> Sorted(const std::set<T>& s) {
    return std::vector<T>(s.begin(), s.end());
}

}  // namespace nxmac::dmsh::py
