#include "py.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>

#include "absl/strings/charconv.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"

namespace nxmac::dmsh::py {
namespace {

// Python's slice bound: a negative one counts from the end, then both are clamped to [0, n]
Int Bound(Int i, Int n) {
    if (i < 0) i += n;
    return std::clamp<Int>(i, 0, n);
}

}  // namespace

std::string_view Slice(std::string_view s, Int a, Int b) {
    const Int n = static_cast<Int>(s.size());
    a = Bound(a, n);
    b = Bound(b, n);
    if (b <= a) return {};
    return s.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
}

Int Find(std::string_view s, std::string_view sub, Int start, Int end) {
    const Int n = static_cast<Int>(s.size());
    start = Bound(start, n);
    end = Bound(end, n);
    if (end - start < static_cast<Int>(sub.size())) return -1;
    const std::string_view part = s.substr(0, static_cast<std::size_t>(end));
    const std::size_t at = part.find(sub, static_cast<std::size_t>(start));
    return at == std::string_view::npos ? -1 : static_cast<Int>(at);
}

Int RFind(std::string_view s, std::string_view sub, Int start, Int end) {
    const Int n = static_cast<Int>(s.size());
    start = Bound(start, n);
    end = Bound(end, n);
    if (end - start < static_cast<Int>(sub.size())) return -1;
    const std::string_view part = s.substr(0, static_cast<std::size_t>(end));
    const std::size_t at = part.rfind(sub);
    if (at == std::string_view::npos || static_cast<Int>(at) < start) return -1;
    return static_cast<Int>(at);
}

Int Index(std::string_view s, std::string_view sub, Int start) {
    const Int at = Find(s, sub, start);
    if (at < 0) throw ValueError("substring not found");
    return at;
}

Int Count(std::string_view s, std::string_view sub) {
    if (sub.empty()) return static_cast<Int>(s.size()) + 1;
    Int n = 0;
    for (std::size_t at = s.find(sub); at != std::string_view::npos; at = s.find(sub, at + sub.size())) ++n;
    return n;
}

std::string Replace(std::string_view s, std::string_view old, std::string_view repl, Int count) {
    std::string out;
    if (old.empty()) {  // Python puts repl between every character, and at both ends
        Int done = 0;
        for (std::size_t i = 0; i <= s.size(); ++i) {
            if (count < 0 || done < count) {
                out += repl;
                ++done;
            }
            if (i < s.size()) out += s[i];
        }
        return out;
    }
    std::size_t at = 0;
    Int done = 0;
    while (count < 0 || done < count) {
        const std::size_t k = s.find(old, at);
        if (k == std::string_view::npos) break;
        out.append(s.substr(at, k - at));
        out += repl;
        at = k + old.size();
        ++done;
    }
    out.append(s.substr(at));
    return out;
}

bool IsSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r') || (c >= '\x1c' && c <= '\x1f'); }

std::string_view LStrip(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && IsSpace(s[i])) ++i;
    return s.substr(i);
}

std::string_view RStrip(std::string_view s) {
    std::size_t n = s.size();
    while (n > 0 && IsSpace(s[n - 1])) --n;
    return s.substr(0, n);
}

std::string_view Strip(std::string_view s) { return RStrip(LStrip(s)); }

std::string_view RStripChars(std::string_view s, std::string_view chars) {
    std::size_t n = s.size();
    while (n > 0 && absl::StrContains(chars, s[n - 1])) --n;
    return s.substr(0, n);
}

std::vector<std::string> Split(std::string_view s, std::string_view sep, Int maxsplit) {
    if (sep.empty()) throw ValueError("empty separator");
    std::vector<std::string> out;
    std::size_t at = 0;
    while (maxsplit < 0 || static_cast<Int>(out.size()) < maxsplit) {
        const std::size_t k = s.find(sep, at);
        if (k == std::string_view::npos) break;
        out.emplace_back(s.substr(at, k - at));
        at = k + sep.size();
    }
    out.emplace_back(s.substr(at));
    return out;
}

std::vector<std::string> SplitWs(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && IsSpace(s[i])) ++i;
        if (i == s.size()) break;
        std::size_t j = i;
        while (j < s.size() && !IsSpace(s[j])) ++j;
        out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::vector<std::string> SplitLines(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t j = i;
        while (j < s.size() &&
               !(s[j] == '\n' || s[j] == '\r' || s[j] == '\v' || s[j] == '\f' || (s[j] >= '\x1c' && s[j] <= '\x1e')))
            ++j;
        out.emplace_back(s.substr(i, j - i));
        if (j == s.size()) break;
        i = j + ((s[j] == '\r' && j + 1 < s.size() && s[j + 1] == '\n') ? 2 : 1);
    }
    return out;
}

std::string Join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string_view RemoveSuffix(std::string_view s, std::string_view suffix) {
    return !suffix.empty() && EndsWith(s, suffix) ? s.substr(0, s.size() - suffix.size()) : s;
}

bool IsDigit(std::string_view s) { return !s.empty() && std::all_of(s.begin(), s.end(), IsDigitChar); }

Int ToInt(std::string_view s, int base) {
    const std::string_view text = Strip(s);
    std::string_view t = text;
    bool neg = false;
    if (!t.empty() && (t[0] == '-' || t[0] == '+')) {
        neg = t[0] == '-';
        t.remove_prefix(1);
    }
    if (base == 0) {
        base = 10;
        if (t.size() > 1 && t[0] == '0') {
            const char p = static_cast<char>(t[1] | 0x20);
            if (p == 'x')
                base = 16;
            else if (p == 'o')
                base = 8;
            else if (p == 'b')
                base = 2;
            if (base != 10) t.remove_prefix(2);
        }
        // Python's base 0 takes no leading zero before other digits ("010"), only zeros alone ("00")
        if (base == 10 && t.size() > 1 && t[0] == '0' && t.find_first_not_of("0_") != std::string_view::npos)
            throw ValueError(absl::StrFormat("invalid literal for int() with base 0: '%s'", text));
    } else if (base == 16 && t.size() > 1 && t[0] == '0' && (t[1] | 0x20) == 'x') {
        t.remove_prefix(2);
    }
    if (t.empty()) throw ValueError(absl::StrFormat("invalid literal for int() with base %d: '%s'", base, text));
    unsigned __int128 v = 0;
    for (char c : t) {
        int d = -1;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
            d = (c | 0x20) - 'a' + 10;
        else if (c == '_')
            continue;
        if (d < 0 || d >= base)
            throw ValueError(absl::StrFormat("invalid literal for int() with base %d: '%s'", base, text));
        v = v * static_cast<unsigned>(base) + static_cast<unsigned>(d);
        if (v > static_cast<unsigned __int128>(std::numeric_limits<Int>::max()) + 1)
            throw ValueError("an integer past 64 bits");
    }
    if (!neg && v > static_cast<unsigned __int128>(std::numeric_limits<Int>::max()))
        throw ValueError("an integer past 64 bits");
    return neg ? static_cast<Int>(-static_cast<__int128>(v)) : static_cast<Int>(v);
}

double ToFloat(std::string_view s) {
    const std::string_view t = Strip(s);
    std::string_view body = t;
    if (!body.empty() && body[0] == '+') body.remove_prefix(1);
    double v = 0;
    const auto [end, ec] = absl::from_chars(body.data(), body.data() + body.size(), v);
    if (body.empty() || end != body.data() + body.size()) {
        throw ValueError(absl::StrFormat("could not convert string to float: '%s'", t));
    }
    if (ec == std::errc::result_out_of_range) v = std::strtod(std::string(body).c_str(), nullptr);
    return v;
}

std::string Hex(Int v) {
    if (v < 0) return absl::StrFormat("-0x%x", static_cast<std::uint64_t>(-static_cast<__int128>(v)));
    return absl::StrFormat("0x%x", static_cast<std::uint64_t>(v));
}

}  // namespace nxmac::dmsh::py
