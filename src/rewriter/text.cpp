#include "text.h"

#include <cstdio>

namespace nxmac::rewriter::internal {
namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHexDigit(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f'); }
bool IsWord(char c) { return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

std::size_t Digits(std::string_view text, std::size_t at) {
    std::size_t n = 0;
    while (at + n < text.size() && IsDigit(text[at + n])) ++n;
    return n;
}

// The length of x18 / w18 style names at `at`, between word boundaries: 0 when there is none
std::size_t RegAt(std::string_view text, std::size_t at) {
    if (text[at] != 'x' && text[at] != 'w') return 0;
    if (at > 0 && IsWord(text[at - 1])) return 0;
    const std::size_t digits = Digits(text, at + 1);
    if (digits == 0) return 0;
    const std::size_t end = at + 1 + digits;
    if (end < text.size() && IsWord(text[end])) return 0;
    return 1 + digits;
}

}  // namespace

std::string HexOf(std::int64_t value) {
    char buffer[32];
    if (value < 0) {
        std::snprintf(buffer, sizeof buffer, "-0x%llx", 0ULL - static_cast<unsigned long long>(value));
    } else {
        std::snprintf(buffer, sizeof buffer, "0x%llx", static_cast<unsigned long long>(value));
    }
    return buffer;
}

std::string HexBare(std::int64_t value) {
    char buffer[32];
    if (value < 0) {
        std::snprintf(buffer, sizeof buffer, "-%llx", 0ULL - static_cast<unsigned long long>(value));
    } else {
        std::snprintf(buffer, sizeof buffer, "%llx", static_cast<unsigned long long>(value));
    }
    return buffer;
}

std::string Hex8(std::uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%08x", value);
    return buffer;
}

std::string BytesHex(const std::uint8_t* data, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (std::size_t i = 0; i < size; ++i) {
        out[2 * i] = kDigits[data[i] >> 4];
        out[2 * i + 1] = kDigits[data[i] & 15];
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> HexBytes(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = nibble(hex[2 * i]);
        const int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return out;
}

bool Scan(std::string_view text, std::string_view pattern, std::initializer_list<std::string_view*> captures) {
    std::size_t at = 0;
    const auto* capture = captures.begin();
    for (std::size_t p = 0; p < pattern.size(); ++p) {
        if (pattern[p] != '%' || p + 1 == pattern.size()) {
            if (at >= text.size() || text[at] != pattern[p]) return false;
            ++at;
            continue;
        }
        const char kind = pattern[++p];
        const std::size_t begin = at;
        if (kind == 'x' || kind == 'w' || kind == 'r') {
            if (at >= text.size()) return false;
            const char c = text[at];
            if (kind == 'r' ? (c != 'x' && c != 'w') : c != kind) return false;
            const std::size_t digits = Digits(text, at + 1);
            if (digits == 0) return false;
            at += 1 + digits;
        } else if (kind == 'n') {
            const std::size_t digits = Digits(text, at);
            if (digits == 0) return false;
            at += digits;
        } else if (kind == 'i') {
            if (at + 2 < text.size() && text[at] == '0' && text[at + 1] == 'x' && IsHexDigit(text[at + 2])) {
                at += 2;
                while (at < text.size() && IsHexDigit(text[at])) ++at;
            } else {
                const std::size_t digits = Digits(text, at);
                if (digits == 0) return false;
                at += digits;
            }
        } else {  // %% and anything else: the character itself
            if (at >= text.size() || text[at] != kind) return false;
            ++at;
            continue;
        }
        if (capture != captures.end()) {
            if (*capture != nullptr) **capture = text.substr(begin, at - begin);
            ++capture;
        }
    }
    return at == text.size();
}

std::int64_t ParseInt(std::string_view text) {
    bool negative = false;
    if (!text.empty() && text[0] == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    std::uint64_t value = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        for (const char c : text.substr(2)) {
            const int digit = IsDigit(c) ? c - '0' : (c | 0x20) - 'a' + 10;
            value = value * 16 + static_cast<std::uint64_t>(digit);
        }
    } else {
        for (const char c : text) value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
}

std::optional<int> RegNum(std::string_view name) {
    if (name.size() < 2 || (name[0] != 'x' && name[0] != 'w')) return std::nullopt;
    if (Digits(name, 1) != name.size() - 1) return std::nullopt;
    return static_cast<int>(ParseInt(name.substr(1)));
}

bool Has18(std::string_view op_str) {
    for (std::size_t at = 0; at < op_str.size(); ++at) {
        if (RegAt(op_str, at) == 3 && op_str[at + 1] == '1' && op_str[at + 2] == '8') return true;
    }
    return false;
}

std::vector<int> RegsNamed(std::string_view op_str) {
    std::vector<int> out;
    for (std::size_t at = 0; at < op_str.size();) {
        const std::size_t length = RegAt(op_str, at);
        if (length == 0) {
            ++at;
            continue;
        }
        out.push_back(static_cast<int>(ParseInt(op_str.substr(at + 1, length - 1))));
        at += length;
    }
    return out;
}

std::string Subst18(std::string_view op_str, int s) {
    std::string out;
    for (std::size_t at = 0; at < op_str.size();) {
        if (RegAt(op_str, at) == 3 && op_str[at + 1] == '1' && op_str[at + 2] == '8') {
            out += op_str[at];
            out += std::to_string(s);
            at += 3;
        } else {
            out += op_str[at++];
        }
    }
    return out;
}

}  // namespace nxmac::rewriter::internal
