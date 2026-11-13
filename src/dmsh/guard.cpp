// c1_guard.py and cb_extent.py: interval arithmetic over a shader's index expressions, for the c1 guard list and the
// uniform read extents. Python's integers are unbounded; here they are 128 bits, and a value past that is unbounded.
#include <functional>
#include <map>

#include "abi.h"
#include "absl/strings/str_cat.h"
#include "modules.h"
#include "re.h"

namespace nxmac::dmsh::guard {
namespace {

using I = __int128;
using Opt = Num;
const Bound kUnbounded{};

bool Known(std::initializer_list<Opt> v) {
    for (const Opt& x : v)
        if (!x) return false;
    return true;
}

struct Overflow : std::exception {};  // a value past 128 bits: unbounded

I Add(I a, I b) {
    I r;
    if (__builtin_add_overflow(a, b, &r)) throw Overflow();
    return r;
}
I Sub(I a, I b) {
    I r;
    if (__builtin_sub_overflow(a, b, &r)) throw Overflow();
    return r;
}
I Mul(I a, I b) {
    I r;
    if (__builtin_mul_overflow(a, b, &r)) throw Overflow();
    return r;
}
I Shl(I a, I b) {
    if (a == 0) return 0;
    if (b >= 126) throw Overflow();
    const I r = a * (static_cast<I>(1) << b);
    if (r / (static_cast<I>(1) << b) != a) throw Overflow();
    return r;
}
I Shr(I a, I b) {  // Python's >>: the floor of a / 2**b
    if (b >= 127) return a < 0 ? -1 : 0;
    return a >> b;  // arithmetic on a negative value (C++20)
}
int BitLength(I v) {  // of a non-negative value
    int n = 0;
    while (v > 0) {
        v >>= 1;
        ++n;
    }
    return n;
}

const std::map<std::string, int>& Binary() {
    static const std::map<std::string, int> b = {{"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4},  {"&", 5},  {"==", 6},
                                                 {"!=", 6}, {"<", 7},  {">", 7}, {"<=", 7}, {">=", 7}, {"<<", 8},
                                                 {">>", 8}, {"+", 9},  {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10}};
    return b;
}

struct ParseError : std::exception {};  // ValueError / IndexError / RecursionError inside expression(): unbounded

std::vector<std::string> Tokens(const std::string& text0) {
    static const re::Pattern as_u(R"(\bas_type<u(?:int|short|long)>\s*\()");
    static const re::Pattern as_any(R"(\bas_type<\w+>\s*\()");
    static const re::Pattern token(
        R"(\s*(0[xX][0-9a-fA-F]+[uU]?|\d+\.\d*(?:[eE][-+]?\d+)?f?|\d+[uU]?|[A-Za-z_]\w*(?:\.\w+)*|\.\w+|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^~!?:(),<>\[\]]))");
    const std::string text = as_any.sub("as_type(", as_u.sub("as_type_u(", text0));
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        auto m = token.match(text, i);
        if (!m) {
            if (!py::Strip(std::string_view(text).substr(i)).empty()) throw ParseError();
            break;
        }
        out.emplace_back(m->group(1));
        i = m->end();
    }
    return out;
}

// int(t.rstrip("uU"), 0), past 64 bits too; ParseError as Python's ValueError (a leading zero before other digits)
I Literal(std::string_view t) {
    t = py::RStripChars(t, "uU");
    int base = 10;
    if (t.size() > 1 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        base = 16;
        t.remove_prefix(2);
    } else if (t.size() > 1 && t[0] == '0' && t.find_first_not_of('0') != std::string_view::npos) {
        throw ParseError();
    }
    I v = 0;
    for (char c : t) {
        const int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
        v = Add(Mul(v, base), d);
    }
    return v;
}

class Evaluator {
public:
    explicit Evaluator(const Definitions& definitions) : definitions_(definitions) {}

    Bound Expression(const std::string& text) {
        try {
            toks_ = Tokens(text);
            pos_ = 0;
            const Bound value = Ternary();
            if (pos_ != toks_.size()) return kUnbounded;
            return value;
        } catch (const ParseError&) {
            return kUnbounded;
        } catch (const Overflow&) {
            return kUnbounded;
        }
    }

private:
    Bound Variable(const std::string& name) {
        if (auto it = memo_.find(name); it != memo_.end()) return it->second;
        auto d = definitions_.find(name);
        if (d == definitions_.end() || depth_ > 32) return kUnbounded;
        memo_[name] = kUnbounded;  // a cycle is unbounded
        ++depth_;
        Bound value;
        try {
            value = Expression(d->second.second);
        } catch (...) {
            --depth_;
            throw;
        }
        --depth_;
        const std::string& kind = d->second.first;
        if (kind == "bool") {
            value = {0, 1};
        } else if (py::StartsWith(kind, "u") && !(value.lo && *value.lo >= 0)) {
            value = {0, Opt()};
        }
        memo_[name] = value;
        return value;
    }

    const std::string* Peek() const { return pos_ < toks_.size() ? &toks_[pos_] : nullptr; }
    std::string Take(std::string_view want = {}) {
        if (pos_ >= toks_.size()) throw ParseError();  // IndexError
        const std::string& t = toks_[pos_];
        if (!want.empty() && t != want) throw ParseError();
        ++pos_;
        return t;
    }

    struct Depth {  // Python's recursion limit, roughly: a deeper expression is unbounded there too
        explicit Depth(int& d) : d_(d) {
            if (++d_ > 400) {
                --d_;
                throw ParseError();
            }
        }
        ~Depth() { --d_; }
        int& d_;
    };

    Bound Ternary() {
        Depth guard(nest_);
        const Bound cond = BinaryExpr(1);
        if (!Peek() || *Peek() != "?") return cond;
        Take("?");
        const Bound a = Ternary();
        Take(":");
        const Bound b = Ternary();
        return {Known({a.lo, b.lo}) ? Opt(std::min(*a.lo, *b.lo)) : Opt(),
                Known({a.hi, b.hi}) ? Opt(std::max(*a.hi, *b.hi)) : Opt()};
    }

    Bound BinaryExpr(int level) {
        Bound left = Unary();
        while (Peek() && Binary().count(*Peek()) && Binary().at(*Peek()) >= level) {
            const std::string op = Take();
            const Bound right = BinaryExpr(Binary().at(op) + 1);
            left = Combine(op, left, right);
        }
        return left;
    }

    static Bound Combine(const std::string& op, const Bound& a, const Bound& b) {
        const Opt al = a.lo, ah = a.hi, bl = b.lo, bh = b.hi;
        if (op == "||" || op == "&&" || op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=")
            return {0, 1};
        if (op == "+")
            return {Known({al, bl}) ? Opt(Add(*al, *bl)) : Opt(), Known({ah, bh}) ? Opt(Add(*ah, *bh)) : Opt()};
        if (op == "-")
            return {Known({al, bh}) ? Opt(Sub(*al, *bh)) : Opt(), Known({ah, bl}) ? Opt(Sub(*ah, *bl)) : Opt()};
        if (op == "*") {
            if (!Known({al, ah, bl, bh})) return kUnbounded;
            const I p[] = {Mul(*al, *bl), Mul(*al, *bh), Mul(*ah, *bl), Mul(*ah, *bh)};
            return {*std::min_element(p, p + 4), *std::max_element(p, p + 4)};
        }
        if (op == "/" || op == ">>" || op == "<<") {
            if (!Known({al, ah, bl, bh}) || *bl != *bh || *bl < 0 || (op == "/" && *bl == 0)) return kUnbounded;
            if (op == "<<") return {Shl(*al, *bl), Shl(*ah, *bl)};
            if (op == ">>") return {Shr(*al, *bl), Shr(*ah, *bl)};
            if (*al >= 0) return {*al / *bl, *ah / *bl};
            return kUnbounded;
        }
        if (op == "%") {
            if (!Known({bl, bh}) || *bl != *bh || *bl <= 0) return kUnbounded;
            if (al && *al >= 0) return {0, *bl - 1};
            return {-(*bl - 1), *bl - 1};
        }
        if (op == "&") {
            std::vector<I> caps;
            for (const Bound& x : {a, b})
                if (Known({x.lo, x.hi}) && *x.lo >= 0) caps.push_back(*x.hi);
            if (caps.empty()) return kUnbounded;
            return {0, *std::min_element(caps.begin(), caps.end())};
        }
        if (op == "|" || op == "^") {
            if (!Known({al, ah, bl, bh}) || *al < 0 || *bl < 0) return kUnbounded;
            const int n = BitLength(std::max(*ah, *bh));
            if (n >= 127) throw Overflow();
            return {0, (static_cast<I>(1) << n) - 1};
        }
        return kUnbounded;
    }

    Bound Unary() {
        Depth guard(nest_);
        const std::string* t = Peek();
        if (t && *t == "-") {
            Take();
            const Bound v = Unary();
            return {v.hi ? Opt(-*v.hi) : Opt(), v.lo ? Opt(-*v.lo) : Opt()};
        }
        if (t && *t == "+") {
            Take();
            return Unary();
        }
        if (t && *t == "!") {
            Take();
            Unary();
            return {0, 1};
        }
        if (t && (*t == "~" || *t == "*")) {
            Take();
            Unary();
            return kUnbounded;
        }
        return Postfix(Primary());
    }

    Bound Postfix(Bound value) {
        while (Peek() && (*Peek() == "[" || py::StartsWith(*Peek(), "."))) {
            if (Take() == "[") {
                Ternary();
                Take("]");
            }
            value = kUnbounded;
        }
        return value;
    }

    Bound Primary() {
        static const re::Pattern integer(R"(0[xX][0-9a-fA-F]+[uU]?|\d+[uU]?)");
        static const re::Pattern real(R"(\d+\.\d*(?:[eE][-+]?\d+)?f?)");
        static const re::Pattern name(R"([A-Za-z_]\w*(?:\.\w+)*)");
        static const re::Pattern tmp(R"(_\d+)");
        const std::string t = Take();
        if (t == "(") {
            const Bound v = Ternary();
            Take(")");
            return v;
        }
        if (integer.fullmatch(t)) {
            const I v = Literal(t);
            return {v, v};
        }
        if (real.fullmatch(t)) return kUnbounded;
        if (!name.fullmatch(t)) throw ParseError();
        if (Peek() && *Peek() == "(") {
            Take("(");
            std::vector<Bound> args;
            if (!Peek() || *Peek() != ")") {
                args.push_back(Ternary());
                while (Peek() && *Peek() == ",") {
                    Take(",");
                    args.push_back(Ternary());
                }
            }
            Take(")");
            return Call(t, args);
        }
        if (tmp.fullmatch(t)) {
            const std::vector<std::string> toks = toks_;
            const std::size_t pos = pos_;
            Bound v;
            try {
                v = Variable(t);
            } catch (...) {
                toks_ = toks;
                pos_ = pos;
                throw;
            }
            toks_ = toks;
            pos_ = pos;
            return v;
        }
        if (t == "true" || t == "false") return {0, 1};
        return kUnbounded;
    }

    static Bound Call(const std::string& name, const std::vector<Bound>& args) {
        static const std::set<std::string> casts = {"int", "uint", "short", "ushort", "long", "ulong"};
        if (name == "as_type_u" && args.size() == 1) return {0, Opt()};
        if (casts.count(name) && args.size() == 1) {
            const auto& [l, h] = args[0];
            if (py::StartsWith(name, "u") && !(l && *l >= 0)) return {0, Opt()};
            return {l, h};
        }
        if (name == "min" && args.size() == 2) {
            const Opt al = args[0].lo, ah = args[0].hi, bl = args[1].lo, bh = args[1].hi;
            std::vector<I> hs;
            for (const Opt& h : {ah, bh})
                if (h) hs.push_back(*h);
            return {Known({al, bl}) ? Opt(std::min(*al, *bl)) : Opt(),
                    hs.empty() ? Opt() : Opt(*std::min_element(hs.begin(), hs.end()))};
        }
        if (name == "max" && args.size() == 2) {
            const Opt al = args[0].lo, ah = args[0].hi, bl = args[1].lo, bh = args[1].hi;
            std::vector<I> ls;
            for (const Opt& l : {al, bl})
                if (l) ls.push_back(*l);
            return {ls.empty() ? Opt() : Opt(*std::max_element(ls.begin(), ls.end())),
                    Known({ah, bh}) ? Opt(std::max(*ah, *bh)) : Opt()};
        }
        if (name == "clamp" && args.size() == 3) return {args[1].lo, args[2].hi};
        return kUnbounded;
    }

    const Definitions& definitions_;
    std::map<std::string, Bound> memo_;
    int depth_ = 0;
    int nest_ = 0;
    std::vector<std::string> toks_;
    std::size_t pos_ = 0;
};

}  // namespace

Definitions DefinitionsOf(const std::string& source) {
    static const re::Pattern assignment(
        R"((?<![\w.])(_\d+)\s*(?:<<|>>|[-+*/%&|^])?=(?!=)|(?<![\w.])(_\d+)\s*(?:\+\+|--)|(?:\+\+|--)\s*(_\d+)\b)");
    static const re::Pattern definition(R"(\b(u?int|u?short|u?long|bool)\s+(_\d+)\s*=\s*([^;]+);)");
    std::map<std::string, int> writes, counts;
    for (const re::Match& m : assignment.finditer(source)) {
        const std::string_view name = m.has(1) && !m.group(1).empty()   ? m.group(1)
                                      : m.has(2) && !m.group(2).empty() ? m.group(2)
                                                                        : m.group(3);
        ++writes[std::string(name)];
    }
    std::map<std::string, std::set<std::pair<std::string, std::string>>> decls;
    for (const re::Match& m : definition.finditer(source)) {
        decls[std::string(m.group(2))].insert({std::string(m.group(1)), std::string(py::Strip(m.group(3)))});
        ++counts[std::string(m.group(2))];
    }
    Definitions out;
    for (const auto& [name, d] : decls) {
        if (d.size() == 1 && writes[name] == counts[name]) out[name] = *d.begin();
    }
    return out;
}

Bound Interval(const Definitions& definitions, const std::string& index) {
    Evaluator i(definitions);
    return i.Expression(index);
}

std::optional<py::Int> C1Span(const Json& refl) {
    if (!refl.is_object()) return std::nullopt;
    const Json* c1 = refl.find("c1");
    const py::Int length = c1 && c1->truthy() ? static_cast<py::Int>(c1->size()) / 2 : 0;
    return (length + 255) / 256 * 256 + 256;
}

bool Unproven(const std::string& source, std::optional<py::Int> span) {
    static const re::Pattern access(R"(\b[a-z]+_c1\)\._m0\[([^\]]+)\])");
    std::set<std::string> accesses;
    for (const std::string& a : access.findall(source)) accesses.insert(a);
    if (accesses.empty()) return false;
    const Definitions definitions = DefinitionsOf(source);
    for (const std::string& index : accesses) {
        const Bound b = Interval(definitions, index);
        const bool proven = span && b.lo && b.hi && *b.lo >= 0 && 16 * (*b.hi + 1) <= *span;
        if (!proven) return true;
    }
    return false;
}

std::map<py::Int, py::Int> Extents(const std::string& source, const Json& bindings) {
    std::map<py::Int, py::Int> out;
    std::optional<Definitions> definitions;
    for (const Json& b : bindings.array()) {
        const Json* kind = b.find("kind");
        if (!kind || !kind->is_string() || kind->as_string() != abi::kKindCbuf || !b.contains("reg")) continue;
        const std::string name = b.at("msl_name").as_string();
        const std::string e = re::escape(name);
        const re::Pattern uses_p(absl::StrCat(R"(\b)", e, R"(\b)"));
        const re::Pattern reads_p(absl::StrCat(R"(\(\*spvDescriptorSet0\.)", e, R"(\)\._m0\[([^\]]+)\])"));
        const re::Pattern declared_p(absl::StrCat(R"(\b)", e, R"(\s*\[\[id\()"));
        const std::size_t uses = uses_p.finditer(source).size();
        const std::vector<std::string> reads = reads_p.findall(source);
        const std::size_t declared = declared_p.finditer(source).size();
        if (uses != reads.size() + declared) continue;
        Num high = -1;
        for (const std::string& index : std::set<std::string>(reads.begin(), reads.end())) {
            if (!definitions) definitions = DefinitionsOf(source);
            const Bound bd = Interval(*definitions, index);
            if (!bd.lo || !bd.hi || *bd.lo < 0) {
                high = Num();
                break;
            }
            high = std::max(*high, *bd.hi);
        }
        if (!high || 16 * (*high + 1) >= 65536) continue;
        out[b.at("reg").as_int()] = static_cast<py::Int>(16 * (*high + 1));
    }
    return out;
}

std::vector<std::optional<py::Int>> Unwritten(const std::string& source, const Json& bindings) {
    static const re::Pattern write(R"(\s*(?:=(?!=)|[-+*/%&|^]=|<<=|>>=|\+\+|--))");
    std::set<py::Int> out;
    bool none = false;
    for (const Json& b : bindings.array()) {
        const Json* kind = b.find("kind");
        if (!kind || !kind->is_string() || kind->as_string() != abi::kKindSbuf || !b.contains("reg")) continue;
        const std::string name = absl::StrCat("spvDescriptorSet0.", b.at("msl_name").as_string());
        const std::string e = re::escape(name);
        bool ok = true;
        const re::Pattern use(absl::StrCat(R"(\(\*)", e, R"(\b)"));
        for (const re::Match& m : use.finditer(source)) {
            std::size_t j = m.end();
            if ((m.start() > 0 && source[m.start() - 1] == '&') || source.compare(j, 6, ")._m0[") != 0) {
                ok = false;
                break;
            }
            j += 5;
            int depth = 0;
            while (j < source.size()) {
                const char c = source[j];
                if (c == '[') {
                    ++depth;
                } else if (c == ']') {
                    --depth;
                } else if (depth == 0 && !(c == '.' || py::IsAlnum(c) || c == '_')) {
                    break;
                }
                ++j;
            }
            if (write.match(source, j)) {
                ok = false;
                break;
            }
        }
        const re::Pattern any(absl::StrCat(e, R"(\b)"));
        if (ok && any.finditer(source).size() == use.finditer(source).size()) {
            const Json& reg = b.at("reg");
            if (reg.is_null())
                none = true;
            else
                out.insert(reg.as_int());
        }
    }
    if (none && !out.empty())
        throw py::Fatal(
            "[RED] cb_extent.py failed (1): TypeError: '<' not supported between instances of 'int' and 'NoneType'");
    std::vector<std::optional<py::Int>> r(out.begin(), out.end());
    if (none) r.emplace_back();  // a slot whose number was not decoded, alone: Python writes it as None
    return r;
}

std::vector<std::string> ExtentTokens(const std::string& source, const Json& bindings) {
    std::vector<std::string> toks;
    for (const auto& [r, n] : Extents(source, bindings)) toks.push_back(absl::StrCat(r, ":", n));
    for (const std::optional<py::Int>& r : Unwritten(source, bindings))
        toks.push_back(r ? absl::StrCat("s", *r) : "sNone");
    return toks;
}

}  // namespace nxmac::dmsh::guard
