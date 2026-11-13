#include "json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>

#include "absl/strings/charconv.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace nxmac::dmsh {
namespace {

[[noreturn]] void Wrong(const char* want) { throw std::logic_error(absl::StrCat("a JSON value that is not ", want)); }

void Quote(std::string& out, std::string_view s, bool ensure_ascii) {
    out += '"';
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':
                out += "\\\"";
                continue;
            case '\\':
                out += "\\\\";
                continue;
            case '\n':
                out += "\\n";
                continue;
            case '\r':
                out += "\\r";
                continue;
            case '\t':
                out += "\\t";
                continue;
            case '\b':
                out += "\\b";
                continue;
            case '\f':
                out += "\\f";
                continue;
            default:
                break;
        }
        if (c < 0x20) {
            absl::StrAppendFormat(&out, "\\u%04x", c);
        } else if (c < 0x80 || !ensure_ascii) {
            out += static_cast<char>(c);
        } else {
            // a UTF-8 sequence as \uXXXX (a surrogate pair past the basic plane)
            int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            std::uint32_t cp = c & (0x3f >> n);
            for (; n > 0 && i + 1 < s.size(); --n) cp = cp << 6 | (static_cast<unsigned char>(s[++i]) & 0x3f);
            if (cp >= 0x10000) {
                cp -= 0x10000;
                absl::StrAppendFormat(&out, "\\u%04x\\u%04x", 0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
            } else {
                absl::StrAppendFormat(&out, "\\u%04x", cp);
            }
        }
    }
    out += '"';
}

struct Style {
    bool indent = false;  // indent=1
    bool ensure_ascii = true;
    bool sort_keys = false;
    std::string_view item = ", ";
    std::string_view key = ": ";
};

void Dump(std::string& out, const Json& v, const Style& st, int level) {
    switch (v.kind()) {
        case Json::Kind::kNull:
            out += "null";
            return;
        case Json::Kind::kBool:
            out += v.as_bool() ? "true" : "false";
            return;
        case Json::Kind::kInt:
            absl::StrAppend(&out, v.as_int());
            return;
        case Json::Kind::kFloat: {
            const double d = v.as_float();
            if (std::isnan(d)) {
                out += "NaN";
            } else if (std::isinf(d)) {
                out += d > 0 ? "Infinity" : "-Infinity";
            } else {
                out += PyFloatRepr(d);
            }
            return;
        }
        case Json::Kind::kString:
            Quote(out, v.as_string(), st.ensure_ascii);
            return;
        case Json::Kind::kArray: {
            const Json::Array& a = v.array();
            if (a.empty()) {
                out += "[]";
                return;
            }
            out += '[';
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (st.indent) {
                    out += i ? ",\n" : "\n";
                    out.append(static_cast<std::size_t>(level) + 1, ' ');
                } else if (i) {
                    out += st.item;
                }
                Dump(out, a[i], st, level + 1);
            }
            if (st.indent) {
                out += '\n';
                out.append(static_cast<std::size_t>(level), ' ');
            }
            out += ']';
            return;
        }
        case Json::Kind::kObject: {
            const Json::Object& o = v.object();
            if (o.empty()) {
                out += "{}";
                return;
            }
            std::vector<const Json::Member*> members;
            members.reserve(o.size());
            for (const Json::Member& m : o) members.push_back(&m);
            if (st.sort_keys) {
                std::stable_sort(members.begin(), members.end(),
                                 [](const auto* a, const auto* b) { return a->first < b->first; });
            }
            out += '{';
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (st.indent) {
                    out += i ? ",\n" : "\n";
                    out.append(static_cast<std::size_t>(level) + 1, ' ');
                } else if (i) {
                    out += st.item;
                }
                Quote(out, members[i]->first, st.ensure_ascii);
                out += st.key;
                Dump(out, members[i]->second, st, level + 1);
            }
            if (st.indent) {
                out += '\n';
                out.append(static_cast<std::size_t>(level), ' ');
            }
            out += '}';
            return;
        }
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    absl::StatusOr<Json> Run() {
        try {
            Space();
            Json v = Value(0);
            Space();
            if (at_ != s_.size()) Fail("extra data");
            return v;
        } catch (const std::invalid_argument& e) {
            return absl::InvalidArgumentError(e.what());
        }
    }

private:
    [[noreturn]] void Fail(const char* why) const {
        throw std::invalid_argument(absl::StrCat("JSON: ", why, " at ", at_));
    }
    void Space() {
        while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' || s_[at_] == '\r')) ++at_;
    }
    bool Take(std::string_view word) {
        if (s_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }
    Json Value(int depth) {
        if (depth > 512) Fail("nested too deep");
        if (at_ >= s_.size()) Fail("a value expected");
        const char c = s_[at_];
        if (c == '{') {
            ++at_;
            Json out = Json::MakeObject();
            Space();
            if (Take("}")) return out;
            for (;;) {
                Space();
                if (at_ >= s_.size() || s_[at_] != '"') Fail("a member name expected");
                std::string key = String();
                Space();
                if (!Take(":")) Fail("':' expected");
                Space();
                out[key] = Value(depth + 1);
                Space();
                if (Take(",")) continue;
                if (Take("}")) return out;
                Fail("',' or '}' expected");
            }
        }
        if (c == '[') {
            ++at_;
            Json out = Json::MakeArray();
            Space();
            if (Take("]")) return out;
            for (;;) {
                Space();
                out.push_back(Value(depth + 1));
                Space();
                if (Take(",")) continue;
                if (Take("]")) return out;
                Fail("',' or ']' expected");
            }
        }
        if (c == '"') return Json(String());
        if (Take("true")) return Json(true);
        if (Take("false")) return Json(false);
        if (Take("null")) return Json();
        if (Take("NaN")) return Json(std::numeric_limits<double>::quiet_NaN());
        if (Take("Infinity")) return Json(std::numeric_limits<double>::infinity());
        if (Take("-Infinity")) return Json(-std::numeric_limits<double>::infinity());
        return Number();
    }
    Json Number() {
        const std::size_t start = at_;
        if (at_ < s_.size() && s_[at_] == '-') ++at_;
        const std::size_t digits = at_;
        while (at_ < s_.size() && s_[at_] >= '0' && s_[at_] <= '9') ++at_;
        if (at_ == digits) Fail("a value expected");
        bool real = false;
        if (at_ < s_.size() && s_[at_] == '.') {
            real = true;
            ++at_;
            while (at_ < s_.size() && s_[at_] >= '0' && s_[at_] <= '9') ++at_;
        }
        if (at_ < s_.size() && (s_[at_] == 'e' || s_[at_] == 'E')) {
            real = true;
            ++at_;
            if (at_ < s_.size() && (s_[at_] == '+' || s_[at_] == '-')) ++at_;
            while (at_ < s_.size() && s_[at_] >= '0' && s_[at_] <= '9') ++at_;
        }
        const std::string_view text = s_.substr(start, at_ - start);
        if (!real) {
            long long v = 0;
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
            if (ec == std::errc() && end == text.data() + text.size()) return Json(v);
            Fail("an integer past 64 bits");
        }
        // float(text): Apple's libc++ has no std::from_chars for double, so Abseil's (locale-free, rounded as
        // Python's); past the range of a double strtod gives what Python does (inf, or 0.0 and the smallest subnormals)
        double v = 0;
        const auto [end, ec] = absl::from_chars(text.data(), text.data() + text.size(), v);
        if (end != text.data() + text.size()) Fail("a number that does not read");
        if (ec == std::errc::result_out_of_range) {
            v = std::strtod(std::string(text).c_str(), nullptr);
        } else if (ec != std::errc()) {
            Fail("a number that does not read");
        }
        return Json(v);
    }
    static void Utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | cp >> 6);
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | cp >> 12);
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | cp >> 18);
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    std::uint32_t Hex4() {
        if (at_ + 4 > s_.size()) Fail("a short \\u escape");
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[at_++];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                Fail("a \\u escape that is not hex digits");
            }
        }
        return v;
    }
    std::string String() {
        ++at_;  // the opening quote
        std::string out;
        for (;;) {
            if (at_ >= s_.size()) Fail("an unterminated string");
            const char c = s_[at_++];
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= s_.size()) Fail("an unterminated string");
            const char e = s_[at_++];
            switch (e) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    std::uint32_t cp = Hex4();
                    if (cp >= 0xd800 && cp < 0xdc00 && s_.substr(at_, 2) == "\\u") {
                        const std::size_t back = at_;
                        at_ += 2;
                        const std::uint32_t low = Hex4();
                        if (low >= 0xdc00 && low < 0xe000) {
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                        } else {
                            at_ = back;
                        }
                    }
                    Utf8(out, cp);
                    break;
                }
                default:
                    Fail("an unknown escape");
            }
        }
    }

    std::string_view s_;
    std::size_t at_ = 0;
};

}  // namespace

Json::Json(unsigned long v) : kind_(Kind::kInt), int_(static_cast<long long>(v)) {
    if (v > static_cast<unsigned long>(std::numeric_limits<long long>::max()))
        throw std::overflow_error("a JSON integer past 63 bits");
}
Json::Json(unsigned long long v) : kind_(Kind::kInt), int_(static_cast<long long>(v)) {
    if (v > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
        throw std::overflow_error("a JSON integer past 63 bits");
}

bool Json::as_bool() const {
    if (kind_ != Kind::kBool) Wrong("a boolean");
    return int_ != 0;
}
long long Json::as_int() const {
    if (kind_ != Kind::kInt && kind_ != Kind::kBool) Wrong("an integer");
    return int_;
}
double Json::as_float() const {
    if (kind_ == Kind::kInt) return static_cast<double>(int_);
    if (kind_ != Kind::kFloat) Wrong("a number");
    return float_;
}
const std::string& Json::as_string() const {
    if (kind_ != Kind::kString) Wrong("a string");
    return string_;
}
const Json::Array& Json::array() const {
    if (kind_ != Kind::kArray) Wrong("a list");
    return array_;
}
Json::Array& Json::array() {
    if (kind_ != Kind::kArray) Wrong("a list");
    return array_;
}
const Json::Object& Json::object() const {
    if (kind_ != Kind::kObject) Wrong("an object");
    return object_;
}
Json::Object& Json::object() {
    if (kind_ != Kind::kObject) Wrong("an object");
    return object_;
}

bool Json::truthy() const {
    switch (kind_) {
        case Kind::kNull:
            return false;
        case Kind::kBool:
        case Kind::kInt:
            return int_ != 0;
        case Kind::kFloat:
            return float_ != 0;
        case Kind::kString:
            return !string_.empty();
        case Kind::kArray:
            return !array_.empty();
        case Kind::kObject:
            return !object_.empty();
    }
    return false;
}

const Json* Json::find(std::string_view key) const {
    if (kind_ != Kind::kObject) Wrong("an object");
    for (const Member& m : object_) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}
Json* Json::find(std::string_view key) { return const_cast<Json*>(std::as_const(*this).find(key)); }

const Json& Json::at(std::string_view key) const {
    const Json* v = find(key);
    if (!v) throw std::out_of_range(absl::StrCat("KeyError: '", key, "'"));
    return *v;
}

Json& Json::operator[](std::string_view key) {
    if (kind_ == Kind::kNull) kind_ = Kind::kObject;
    if (Json* v = find(key)) return *v;
    object_.emplace_back(std::string(key), Json());
    return object_.back().second;
}

bool Json::erase(std::string_view key) {
    if (kind_ != Kind::kObject) Wrong("an object");
    for (auto it = object_.begin(); it != object_.end(); ++it) {
        if (it->first == key) {
            object_.erase(it);
            return true;
        }
    }
    return false;
}

void Json::update(const Json& other) {
    for (const Member& m : other.object()) (*this)[m.first] = m.second;
}

void Json::push_back(Json value) {
    if (kind_ == Kind::kNull) kind_ = Kind::kArray;
    if (kind_ != Kind::kArray) Wrong("a list");
    array_.push_back(std::move(value));
}

std::size_t Json::size() const {
    switch (kind_) {
        case Kind::kString:
            return string_.size();
        case Kind::kArray:
            return array_.size();
        case Kind::kObject:
            return object_.size();
        default:
            Wrong("a list, an object or a string");
    }
}

bool operator==(const Json& a, const Json& b) {
    if (a.is_number() && b.is_number() && a.kind_ != b.kind_) return a.as_float() == b.as_float();
    if (a.kind_ != b.kind_) {
        // Python: True == 1
        const bool ints = (a.kind_ == Json::Kind::kBool || a.kind_ == Json::Kind::kInt) &&
                          (b.kind_ == Json::Kind::kBool || b.kind_ == Json::Kind::kInt);
        return ints && a.int_ == b.int_;
    }
    switch (a.kind_) {
        case Json::Kind::kNull:
            return true;
        case Json::Kind::kBool:
        case Json::Kind::kInt:
            return a.int_ == b.int_;
        case Json::Kind::kFloat:
            return a.float_ == b.float_;
        case Json::Kind::kString:
            return a.string_ == b.string_;
        case Json::Kind::kArray:
            return a.array_ == b.array_;
        case Json::Kind::kObject: {
            // dicts compare without regard to order
            if (a.object_.size() != b.object_.size()) return false;
            for (const Json::Member& m : a.object_) {
                const Json* other = b.find(m.first);
                if (!other || !(*other == m.second)) return false;
            }
            return true;
        }
    }
    return false;
}

std::string PyFloatRepr(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";
    // the shortest digits that read back as v, then Python's layout of them (float_repr_style 'short')
    char buffer[64];
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof buffer, v, std::chars_format::scientific);
    std::string_view text(buffer, static_cast<std::size_t>(end - buffer));
    std::string out;
    if (text.front() == '-') {
        out += '-';
        text.remove_prefix(1);
    }
    const std::size_t e = text.find('e');
    std::string digits;
    for (const char c : text.substr(0, e)) {
        if (c != '.') digits += c;
    }
    int exponent = 0;
    std::from_chars(text.data() + e + 1 + (text[e + 1] == '+' ? 1 : 0), text.data() + text.size(), exponent);
    const int decpt = exponent + 1;  // the value is 0.digits x 10^decpt
    const int n = static_cast<int>(digits.size());
    if (decpt > -4 && decpt <= 16) {
        if (decpt <= 0) {
            out += "0.";
            out.append(static_cast<std::size_t>(-decpt), '0');
            out += digits;
        } else if (decpt >= n) {
            out += digits;
            out.append(static_cast<std::size_t>(decpt - n), '0');
            out += ".0";
        } else {
            out += digits.substr(0, static_cast<std::size_t>(decpt));
            out += '.';
            out += digits.substr(static_cast<std::size_t>(decpt));
        }
        return out;
    }
    out += digits[0];
    if (n > 1) {
        out += '.';
        out += digits.substr(1);
    }
    absl::StrAppendFormat(&out, "e%c%02d", exponent < 0 ? '-' : '+', exponent < 0 ? -exponent : exponent);
    return out;
}

std::string JsonIndent1(const Json& value) {
    std::string out;
    Style st;
    st.indent = true;
    st.ensure_ascii = false;
    Dump(out, value, st, 0);
    return out;
}

std::string JsonLine(const Json& value, bool ensure_ascii) {
    std::string out;
    Style st;
    st.ensure_ascii = ensure_ascii;
    Dump(out, value, st, 0);
    return out;
}

std::string JsonSortedCompact(const Json& value, bool ensure_ascii) {
    std::string out;
    Style st;
    st.ensure_ascii = ensure_ascii;
    st.sort_keys = true;
    st.item = ",";
    st.key = ":";
    Dump(out, value, st, 0);
    return out;
}

absl::StatusOr<Json> JsonParse(std::string_view text) { return Parser(text).Run(); }

}  // namespace nxmac::dmsh
