#include "re.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

namespace nxmac::dmsh::re {
namespace {

// A Python pattern as PCRE2 reads the same thing: \Z is \z there, and a [ inside a character class is a literal (PCRE2
// would start a POSIX class at [: [. or [=)
std::string Translate(std::string_view p) {
    std::string out;
    out.reserve(p.size() + 8);
    bool in_class = false;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const char c = p[i];
        if (c == '\\' && i + 1 < p.size()) {
            const char n = p[i + 1];
            if (n == 'Z' && !in_class) {
                out += "\\z";
            } else {
                out += c;
                out += n;
            }
            ++i;
            continue;
        }
        if (in_class) {
            if (c == '[') {
                out += "\\[";
            } else {
                out += c;
                if (c == ']') in_class = false;
            }
            continue;
        }
        out += c;
        if (c == '[') {
            in_class = true;
            if (i + 1 < p.size() && p[i + 1] == '^') out += p[++i];
            if (i + 1 < p.size() && p[i + 1] == ']') out += p[++i];  // a ] that comes first is a member
        }
    }
    return out;
}

// What a thread matches with: no limit on the search (Python has none), and a stack for the JIT's code
struct ThreadState {
    pcre2_match_context* context = nullptr;
    pcre2_jit_stack* stack = nullptr;
    ThreadState() {
        context = pcre2_match_context_create(nullptr);
        pcre2_set_match_limit(context, 0xffffffffu);
        pcre2_set_depth_limit(context, 0xffffffffu);
        pcre2_set_heap_limit(context, 0xffffffffu);
        stack = pcre2_jit_stack_create(64 * 1024, 512 * 1024 * 1024, nullptr);
        if (stack) pcre2_jit_stack_assign(context, nullptr, stack);
    }
    ~ThreadState() {
        if (stack) pcre2_jit_stack_free(stack);
        pcre2_match_context_free(context);
    }
    ThreadState(const ThreadState&) = delete;
    ThreadState& operator=(const ThreadState&) = delete;
};

ThreadState& State() {
    thread_local ThreadState state;
    return state;
}

std::string ErrorText(int code) {
    PCRE2_UCHAR buffer[256];
    pcre2_get_error_message(code, buffer, sizeof buffer);
    return reinterpret_cast<const char*>(buffer);
}

}  // namespace

namespace internal {
struct Code {
    pcre2_code* code = nullptr;
    bool jit = false;
    std::string source;
    Code() = default;
    Code(const Code&) = delete;
    Code& operator=(const Code&) = delete;
    ~Code() {
        if (code) pcre2_code_free(code);
    }
};
}  // namespace internal

namespace {

int GroupNumber(const internal::Code& code, std::string_view name) {
    const std::string n(name);
    const int number = pcre2_substring_number_from_name(code.code, reinterpret_cast<PCRE2_SPTR>(n.c_str()));
    return number < 0 ? -1 : number;
}

// The patterns of the one-off forms, kept as Python's re keeps the last ones it compiled
std::shared_ptr<const Pattern> Cached(std::string_view pattern, unsigned flags) {
    static std::mutex mu;
    static auto* const cache = new std::unordered_map<std::string, std::shared_ptr<const Pattern>>();
    std::string key = absl::StrCat(flags, "\n", pattern);
    const std::lock_guard<std::mutex> lock(mu);
    const auto it = cache->find(key);
    if (it != cache->end()) return it->second;
    if (cache->size() >= 4096) cache->clear();
    auto made = std::make_shared<const Pattern>(pattern, flags);
    cache->emplace(std::move(key), made);
    return made;
}

}  // namespace

Pattern::Pattern(std::string_view pattern, unsigned flags) : code_(std::make_shared<internal::Code>()) {
    const std::string translated = Translate(pattern);
    // Python's ^ in multiline mode matches after a newline that ends the text too
    std::uint32_t options = PCRE2_ALT_CIRCUMFLEX;
    if (flags & I) options |= PCRE2_CASELESS;
    if (flags & M) options |= PCRE2_MULTILINE;
    if (flags & S) options |= PCRE2_DOTALL;
    if (flags & X) options |= PCRE2_EXTENDED;
    int error = 0;
    PCRE2_SIZE offset = 0;
    code_->source = std::string(pattern);
    code_->code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(translated.data()), translated.size(), options, &error,
                                &offset, nullptr);
    if (!code_->code) {
        throw std::invalid_argument(
            absl::StrCat("regular expression ", pattern, ": ", ErrorText(error), " at ", offset));
    }
    code_->jit = pcre2_jit_compile(code_->code, PCRE2_JIT_COMPLETE) == 0;
    std::uint32_t count = 0;
    pcre2_pattern_info(code_->code, PCRE2_INFO_CAPTURECOUNT, &count);
    groups_ = static_cast<int>(count);
}

Pattern::~Pattern() = default;
Pattern::Pattern(Pattern&&) noexcept = default;

int Pattern::group_number(std::string_view name) const { return GroupNumber(*code_, name); }

std::optional<Match> Pattern::Run(std::string_view s, std::size_t pos, std::size_t endpos, unsigned options) const {
    const std::size_t length = std::min(endpos, s.size());
    if (pos > length) return std::nullopt;
    ThreadState& state = State();
    pcre2_match_data* data = pcre2_match_data_create_from_pattern(code_->code, nullptr);
    if (!data) throw std::bad_alloc();
    const auto* subject = reinterpret_cast<PCRE2_SPTR>(s.data() ? s.data() : "");
    int rc = pcre2_match(code_->code, subject, length, pos, options, data, state.context);
    if (rc == PCRE2_ERROR_JIT_STACKLIMIT) {  // deeper than the JIT's stack: the interpreter, which has no such bound
        rc = pcre2_match(code_->code, subject, length, pos, options | PCRE2_NO_JIT, data, state.context);
    }
    if (rc == PCRE2_ERROR_NOMATCH) {
        pcre2_match_data_free(data);
        return std::nullopt;
    }
    if (rc < 0) {
        pcre2_match_data_free(data);
        throw std::runtime_error(absl::StrCat("regular expression ", code_->source, ": ", ErrorText(rc)));
    }
    Match m;
    m.code_ = code_;
    m.groups_ = groups_;
    m.subject_ = s.substr(0, length);
    const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(data);
    const std::size_t pairs = static_cast<std::size_t>(groups_) + 1;
    m.spans_.assign(2 * pairs, npos);
    const std::size_t set = rc == 0 ? pairs : static_cast<std::size_t>(rc);
    for (std::size_t i = 0; i < std::min(set, pairs); ++i) {
        if (ovector[2 * i] == PCRE2_UNSET) continue;
        m.spans_[2 * i] = ovector[2 * i];
        m.spans_[2 * i + 1] = ovector[2 * i + 1];
    }
    // \K could put the start after the end; Python has no \K, and no ported pattern uses it
    pcre2_match_data_free(data);
    return m;
}

std::optional<Match> Pattern::search(std::string_view s, std::size_t pos, std::size_t endpos) const {
    return Run(s, pos, endpos, 0);
}

std::optional<Match> Pattern::match(std::string_view s, std::size_t pos, std::size_t endpos) const {
    return Run(s, pos, endpos, PCRE2_ANCHORED);
}

std::optional<Match> Pattern::fullmatch(std::string_view s, std::size_t pos, std::size_t endpos) const {
    return Run(s, pos, endpos, PCRE2_ANCHORED | PCRE2_ENDANCHORED);
}

std::vector<Match> Pattern::finditer(std::string_view s, std::size_t pos, std::size_t endpos) const {
    std::vector<Match> out;
    const std::size_t length = std::min(endpos, s.size());
    bool must_advance = false;  // the last match was empty: no empty match where it was
    while (pos <= length) {
        std::optional<Match> m = Run(s, pos, endpos, must_advance ? PCRE2_NOTEMPTY_ATSTART : 0);
        if (!m) break;
        must_advance = m->end() == m->start();
        pos = m->end();
        out.push_back(std::move(*m));
    }
    return out;
}

std::string Pattern::sub(const Replace& repl, std::string_view s, std::size_t count, std::size_t* n) const {
    std::string out;
    std::size_t at = 0, pos = 0, done = 0;
    bool must_advance = false;
    while (pos <= s.size() && (count == 0 || done < count)) {
        const std::optional<Match> m = Run(s, pos, npos, must_advance ? PCRE2_NOTEMPTY_ATSTART : 0);
        if (!m) break;
        out.append(s.substr(at, m->start() - at));
        out.append(repl(*m));
        at = m->end();
        must_advance = m->end() == m->start();
        pos = m->end();
        ++done;
    }
    out.append(s.substr(at));
    if (n) *n = done;
    return out;
}

std::string Pattern::sub(std::string_view tmpl, std::string_view s, std::size_t count, std::size_t* n) const {
    if (!absl::StrContains(tmpl, '\\')) {
        return sub([&](const Match&) { return std::string(tmpl); }, s, count, n);
    }
    return sub([&](const Match& m) { return m.expand(tmpl); }, s, count, n);
}

std::vector<std::string> Pattern::split(std::string_view s) const {
    std::vector<std::string> out;
    std::size_t at = 0;
    for (const Match& m : finditer(s)) {
        out.emplace_back(s.substr(at, m.start() - at));
        for (int g = 1; g <= groups_; ++g) out.emplace_back(m.group(g));
        at = m.end();
    }
    out.emplace_back(s.substr(at));
    return out;
}

std::vector<std::string> Pattern::findall(std::string_view s) const {
    if (groups_ > 1) throw std::logic_error("findall of a pattern with several groups: use finditer");
    std::vector<std::string> out;
    for (const Match& m : finditer(s)) out.emplace_back(m.group(groups_));
    return out;
}

std::string_view Match::group(int i) const {
    if (!has(i)) return {};
    const auto k = static_cast<std::size_t>(i);
    return subject_.substr(spans_[2 * k], spans_[2 * k + 1] - spans_[2 * k]);
}

bool Match::has(int i) const {
    if (i < 0 || static_cast<std::size_t>(i) * 2 >= spans_.size()) throw std::out_of_range("no such group");
    return spans_[2 * static_cast<std::size_t>(i)] != Pattern::npos;
}

std::string_view Match::group(std::string_view name) const {
    const int n = GroupNumber(*code_, name);
    if (n < 0) throw std::out_of_range(absl::StrCat("no such group: ", name));
    return group(n);
}

bool Match::has(std::string_view name) const {
    const int n = GroupNumber(*code_, name);
    if (n < 0) throw std::out_of_range(absl::StrCat("no such group: ", name));
    return has(n);
}

std::size_t Match::start(int i) const { return has(i) ? spans_[2 * static_cast<std::size_t>(i)] : Pattern::npos; }
std::size_t Match::end(int i) const { return has(i) ? spans_[2 * static_cast<std::size_t>(i) + 1] : Pattern::npos; }
int Match::groups() const { return groups_; }

std::string Match::expand(std::string_view tmpl) const {
    std::string out;
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        const char c = tmpl[i];
        if (c != '\\' || i + 1 >= tmpl.size()) {
            out += c;
            continue;
        }
        const char n = tmpl[++i];
        if (n == 'g' && i + 1 < tmpl.size() && tmpl[i + 1] == '<') {
            const std::size_t close = tmpl.find('>', i + 2);
            if (close == std::string_view::npos) throw std::invalid_argument("a replacement's \\g< without >");
            const std::string_view name = tmpl.substr(i + 2, close - i - 2);
            int number = 0;
            bool digits = !name.empty();
            for (const char d : name) digits = digits && d >= '0' && d <= '9';
            if (digits) {
                for (const char d : name) number = number * 10 + (d - '0');
            } else {
                number = GroupNumber(*code_, name);
                if (number < 0) throw std::invalid_argument(absl::StrCat("a replacement names no group: ", name));
            }
            out.append(group(number));
            i = close;
        } else if (n >= '0' && n <= '9') {
            // \1 .. \99 (Python reads \0 and three octal digits as a character, which no ported template uses)
            int number = n - '0';
            if (i + 1 < tmpl.size() && tmpl[i + 1] >= '0' && tmpl[i + 1] <= '9' &&
                number * 10 + (tmpl[i + 1] - '0') <= groups()) {
                number = number * 10 + (tmpl[++i] - '0');
            }
            out.append(group(number));
        } else if (n == 'n') {
            out += '\n';
        } else if (n == 't') {
            out += '\t';
        } else if (n == 'r') {
            out += '\r';
        } else if (n == '\\') {
            out += '\\';
        } else {
            // Python refuses an unknown escape of a letter and keeps the backslash before anything else
            out += '\\';
            out += n;
        }
    }
    return out;
}

std::optional<Match> search(std::string_view pattern, std::string_view s, unsigned flags) {
    return Cached(pattern, flags)->search(s);
}

std::optional<Match> match(std::string_view pattern, std::string_view s, unsigned flags) {
    return Cached(pattern, flags)->match(s);
}

std::optional<Match> fullmatch(std::string_view pattern, std::string_view s, unsigned flags) {
    return Cached(pattern, flags)->fullmatch(s);
}

std::string sub(std::string_view pattern, std::string_view tmpl, std::string_view s, std::size_t count,
                unsigned flags) {
    return Cached(pattern, flags)->sub(tmpl, s, count);
}

std::string sub(std::string_view pattern, const Replace& repl, std::string_view s, std::size_t count, unsigned flags) {
    return Cached(pattern, flags)->sub(repl, s, count);
}

std::string escape(std::string_view text) {
    // Python 3.7+: only the characters that can mean something in a pattern
    static constexpr std::string_view special = "()[]{}?*+-|^$\\.&~# \t\n\r\v\f";
    std::string out;
    for (const char c : text) {
        if (absl::StrContains(special, c)) out += '\\';
        out += c;
    }
    return out;
}

}  // namespace nxmac::dmsh::re
