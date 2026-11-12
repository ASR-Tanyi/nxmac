// Python's re, as the shader build uses it (internal to nxmac::dmsh). nxnative's dmsh_build.py and its modules rewrite
// Metal source with regular expressions; their ports keep each pattern as it is written there and match it with PCRE2,
// whose rules are Perl's, as Python's are (the leftmost match, alternatives in order, greedy and lazy quantifiers by
// backtracking). What differs between the two is handled here:
//   - \Z in a pattern is Python's end of the text (PCRE2's \z);
//   - search / match / fullmatch at a position see the whole text (^ and lookbehind as in Python's pattern.search(s, pos));
//   - finditer and sub step as Python 3.7+ does: an empty match is taken beside a match that was not empty, and never
//     twice at one place;
//   - a replacement template is Python's: \1, \g<1>, \g<name>, \\, \n; a group that took no part gives nothing.
// The source is taken as bytes (the Metal source is ASCII), and no limit stops a long match: Python has none.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxmac::dmsh::re {

enum Flags : unsigned {
    kNone = 0,
    I = 1,  // re.I
    M = 2,  // re.M
    S = 4,  // re.S
    X = 8,  // re.X
};

class Pattern;
namespace internal {
struct Code;  // a compiled pattern
}

class Match {
public:
    // The text of group i (0: the whole match); empty for a group that took no part (Python's None: has() tells)
    std::string_view group(int i = 0) const;
    std::string_view group(std::string_view name) const;
    bool has(int i) const;
    bool has(std::string_view name) const;
    std::size_t start(int i = 0) const;
    std::size_t end(int i = 0) const;
    // The number of groups of the pattern (m.re.groups)
    int groups() const;
    // m.expand(template)
    std::string expand(std::string_view tmpl) const;
    std::string_view string() const { return subject_; }

private:
    friend class Pattern;
    std::shared_ptr<const internal::Code> code_;  // a match outlives the pattern that made it
    int groups_ = 0;
    std::string_view subject_;
    std::vector<std::size_t> spans_;  // start, end of each group; npos for one that took no part
};

using Replace = std::function<std::string(const Match&)>;

class Pattern {
public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    // Throws std::invalid_argument for a pattern PCRE2 does not compile (a mistake in the port, never in the data)
    explicit Pattern(std::string_view pattern, unsigned flags = kNone);
    ~Pattern();
    Pattern(const Pattern&) = delete;
    Pattern& operator=(const Pattern&) = delete;
    Pattern(Pattern&&) noexcept;

    std::optional<Match> search(std::string_view s, std::size_t pos = 0, std::size_t endpos = npos) const;
    std::optional<Match> match(std::string_view s, std::size_t pos = 0, std::size_t endpos = npos) const;
    std::optional<Match> fullmatch(std::string_view s, std::size_t pos = 0, std::size_t endpos = npos) const;
    // Every match, left to right, as pattern.finditer(s, pos)
    std::vector<Match> finditer(std::string_view s, std::size_t pos = 0, std::size_t endpos = npos) const;
    // pattern.sub(repl, s, count): count 0 replaces all. n (when given) gets the number of replacements (subn)
    std::string sub(std::string_view tmpl, std::string_view s, std::size_t count = 0, std::size_t* n = nullptr) const;
    std::string sub(const Replace& repl, std::string_view s, std::size_t count = 0, std::size_t* n = nullptr) const;
    // pattern.split(s): the pieces between matches, with the groups' texts between them as Python gives them (an
    // unmatched group as an empty piece)
    std::vector<std::string> split(std::string_view s) const;
    // pattern.findall(s) for a pattern with at most one group: that group's text of every match, or the whole match
    std::vector<std::string> findall(std::string_view s) const;

    int groups() const { return groups_; }
    // The number of a named group, -1 when the pattern has none of that name
    int group_number(std::string_view name) const;

private:
    std::optional<Match> Run(std::string_view s, std::size_t pos, std::size_t endpos, unsigned options) const;

    std::shared_ptr<internal::Code> code_;
    int groups_ = 0;
};

// The one-off forms, re.search(pattern, s) and the like: the compiled pattern is kept for the next call, as Python
// keeps it
std::optional<Match> search(std::string_view pattern, std::string_view s, unsigned flags = kNone);
std::optional<Match> match(std::string_view pattern, std::string_view s, unsigned flags = kNone);
std::optional<Match> fullmatch(std::string_view pattern, std::string_view s, unsigned flags = kNone);
std::string sub(std::string_view pattern, std::string_view tmpl, std::string_view s, std::size_t count = 0,
                unsigned flags = kNone);
std::string sub(std::string_view pattern, const Replace& repl, std::string_view s, std::size_t count = 0,
                unsigned flags = kNone);

// re.escape
std::string escape(std::string_view text);

}  // namespace nxmac::dmsh::re
