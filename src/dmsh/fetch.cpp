// dmsh_fetch.py: a pixel shader's readable-tile version (fold), the selects of bounded uniform reads (select_guards)
// and a vertex shader's screen varyings (screen_varyings).
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "abi.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_replace.h"
#include "modules.h"
#include "re.h"

namespace nxmac::dmsh::fetch {
namespace {

using py::Int;
constexpr int kNfb = 8;

const std::string& GSize() {
    static const std::string s =
        absl::StrFormat(R"(\(\(\*spvDescriptorSet0\.[a-z]+_c%d\)\._m0\[\d\]\.[xyzw] >> 2u\))", abi::kCbsizeBank);
    return s;
}

std::string GChoice(std::string_view cond, std::string_view read) {
    const std::string zero = R"(0(?:\.0|u)?)";
    std::vector<std::string> forms;
    const std::pair<int, std::string_view> fs[] = {{1, R"((?<![\w>])\((?P<q%d>%s) \? (?P<r%d>%s) : (?P<z%d>%s)\))"},
                                                   {2, R"((?P<q%d>%s) \? (?P<r%d>%s) : (?P<z%d>%s)(?![\w.]))"}};
    for (const auto& [n, form] : fs) {
        const std::string r = absl::StrReplaceAll(read, {{"%s", absl::StrCat(n)}});
        std::string f(form);
        // form % (n, cond, n, read % n, n, zero)
        const std::string values[] = {absl::StrCat(n), std::string(cond), absl::StrCat(n), r, absl::StrCat(n), zero};
        std::string out;
        int k = 0;
        for (std::size_t i = 0; i < f.size(); ++i) {
            if (f[i] == '%' && i + 1 < f.size() && (f[i + 1] == 'd' || f[i + 1] == 's')) {
                out += values[k++];
                ++i;
            } else {
                out += f[i];
            }
        }
        forms.push_back(out);
    }
    return py::Join(forms, "|");
}

const std::string_view kScale = "(*spvDescriptorSet0.support_buffer)._m5[0].x";

// _lin's linear forms: {symbol: coefficient}, '1' the constant
using Form = std::map<std::string, double>;
using Env = std::function<std::optional<Form>(const std::string&)>;

struct LinError : std::exception {};  // ValueError / ZeroDivisionError inside _lin: the expression has no form

class Lin {
public:
    Lin(std::vector<std::string> toks, Env env) : toks_(std::move(toks)), env_(std::move(env)) {}

    std::optional<Form> Run() {
        try {
            Form v = AddExpr();
            if (Peek()) return std::nullopt;
            return v;
        } catch (const LinError&) {
            return std::nullopt;
        }
    }

private:
    static Form Add(const Form& a, const Form& b, double s = 1.0) {
        Form r = a;
        for (const auto& [k, v] : b) r[k] = (r.count(k) ? r[k] : 0.0) + s * v;
        Form out;
        for (const auto& [k, v] : r)
            if (v != 0.0) out[k] = v;
        return out;
    }
    static bool Const(const Form& a) {
        for (const auto& [k, v] : a)
            if (k != "1") return false;
        return true;
    }
    static Form Scale(const Form& a, double c) {
        Form out;
        for (const auto& [k, v] : a)
            if (v * c != 0.0) out[k] = v * c;
        return out;
    }
    static double One(const Form& a) {
        auto it = a.find("1");
        return it == a.end() ? 0.0 : it->second;
    }
    const std::string* Peek() const { return i_ < toks_.size() ? &toks_[i_] : nullptr; }
    std::string Take(std::string_view t = {}) {
        const std::string* x = Peek();
        if (!x || (!t.empty() && *x != t)) throw LinError();
        ++i_;
        return *x;
    }
    Form Atom() {
        const std::string t = Take();
        if (t == "-") return Scale(Unary(), -1.0);
        if (t == "(") {
            Form v = AddExpr();
            Take(")");
            return v;
        }
        if (t == "fma") {
            Take("(");
            Form a = AddExpr();
            Take(",");
            Form b = AddExpr();
            Take(",");
            Form c = AddExpr();
            Take(")");
            return Add(Mul(a, b), c);
        }
        if (py::StartsWith(t, "#")) {
            const double f = py::ToFloat(t.substr(1));
            return f != 0.0 ? Form{{"1", f}} : Form{};
        }
        std::optional<Form> v = env_(t);
        if (!v) throw LinError();
        return *v;
    }
    Form Unary() { return Atom(); }
    static Form Mul(const Form& a, const Form& b) {
        if (Const(a)) return Scale(b, One(a));
        if (Const(b)) return Scale(a, One(b));
        throw LinError();
    }
    Form MulExpr() {
        Form v = Unary();
        while (Peek() && (*Peek() == "*" || *Peek() == "/")) {
            const std::string op = Take();
            Form w = Unary();
            if (op == "*") {
                v = Mul(v, w);
            } else if (Const(w) && One(w) != 0.0) {
                v = Scale(v, 1.0 / w.at("1"));
            } else {
                throw LinError();
            }
        }
        return v;
    }
    Form AddExpr() {
        Form v = MulExpr();
        while (Peek() && (*Peek() == "+" || *Peek() == "-")) {
            const std::string op = Take();
            v = Add(v, MulExpr(), op == "+" ? 1.0 : -1.0);
        }
        return v;
    }

    std::vector<std::string> toks_;
    Env env_;
    std::size_t i_ = 0;
};

std::optional<Form> LinOf(const std::string& expr, const Env& env) {
    static const re::Pattern tok(
        R"(\s*(?:(out\.gl_Position\.[xyzw])|(_\d+)|(\d+\.\d*(?:e[+-]?\d+)?|\d+)f?|(fma)|([-+*/(),])))");
    std::vector<std::string> toks;
    std::size_t pos = 0;
    while (pos < expr.size()) {
        auto m = tok.match(expr, pos);
        if (!m || m->end() == pos) return std::nullopt;
        std::string t;
        if (m->has(1) && !m->group(1).empty())
            t = m->group(1);
        else if (m->has(2) && !m->group(2).empty())
            t = m->group(2);
        else if (m->has(3) && !m->group(3).empty())
            t = absl::StrCat("#", m->group(3));
        else if (m->has(4) && !m->group(4).empty())
            t = m->group(4);
        else if (m->has(5) && !m->group(5).empty())
            t = m->group(5);
        if (!t.empty()) toks.push_back(t);
        pos = m->end();
    }
    return Lin(std::move(toks), env).Run();
}

}  // namespace

bool Once(std::string_view msl, std::string_view name) {
    const std::string text = absl::StrCat(name, " = ");
    int n = 0;
    Int i = py::Find(msl, text);
    while (i >= 0) {
        if (!i || !(msl[i - 1] == '.' || msl[i - 1] == '_' || py::IsAlnum(msl[i - 1]))) ++n;
        i = py::Find(msl, text, i + static_cast<Int>(text.size()));
    }
    return n == 1;
}

std::optional<std::string> Proj(const std::string& msl, std::string_view var, std::string_view comp,
                                const std::map<std::string, int>& locn) {
    auto src = [&](std::string_view n, std::string_view c) -> std::optional<std::string> {
        auto m = re::search(absl::StrFormat(R"(float %s = in\.(m_\d+)\.%s;)", re::escape(n), c), msl);
        if (m && Once(msl, n) && locn.count(std::string(m->group(1)))) return std::string(m->group(1));
        return std::nullopt;
    };
    if (!Once(msl, var)) return std::nullopt;
    if (auto m = re::search(absl::StrFormat(R"(float %s = (_\d+) \* (_\d+);)", re::escape(var)), msl)) {
        const std::string g1(m->group(1)), g2(m->group(2));
        for (const auto& [x, r] : {std::pair{g1, g2}, std::pair{g2, g1}}) {
            auto d = re::search(absl::StrFormat(R"(float %s = 1\.0 / (_\d+);)", re::escape(r)), msl);
            if (d && Once(msl, r)) {
                const auto sx = src(x, comp);
                if (sx && sx == src(d->group(1), "w")) return sx;
            }
        }
        return std::nullopt;
    }
    auto m = re::search(absl::StrFormat(R"(float %s = (_\d+) / (_\d+);)", re::escape(var)), msl);
    if (!m) return std::nullopt;
    std::optional<std::string> a = src(m->group(1), comp);
    if (a == src(m->group(2), "w")) return a;
    return std::nullopt;
}

std::string SelectGuards(const std::string& msl_in) {
    if (!py::Contains(msl_in, "spvDescriptorSet0")) return msl_in;
    static const re::Pattern gbool(absl::StrFormat(R"(\bbool (_\d+) = [^;\n]* < %s;)", GSize()));
    static const re::Pattern grow(GChoice(
        R"(_\d+)",
        R"(\(\*spvDescriptorSet0\.[a-z]+_c\d+\)\._m0\[(?P<i%s>_\d+) \? [^\]\n]+ : 0\](?:\[[^\]\n]+\]|\.[xyzw]))"));
    static const re::Pattern gleft(absl::StrFormat(R"(%s\) \?)", GSize()));
    static const re::Pattern cond_use(R"((?<!\[)\b(_\d+) \?)");
    std::set<std::string> bools;
    for (const std::string& b : gbool.findall(msl_in)) bools.insert(b);
    std::string msl = msl_in;
    if (!bools.empty()) {
        msl = grow.sub(
            [&](const re::Match& m) {
                const std::string n = m.has("q1") ? "1" : "2";
                const std::string c(m.group("q" + n));
                if (!bools.count(c) || m.group("i" + n) != c) return std::string(m.group(0));
                return absl::StrFormat("select(%s, %s, %s)", m.group("z" + n), m.group("r" + n), c);
            },
            msl);
    }
    std::optional<re::Match> left = gleft.search(msl);
    if (!left) {
        for (re::Match& m : cond_use.finditer(msl)) {
            if (bools.count(std::string(m.group(1)))) {
                left = m;
                break;
            }
        }
    }
    if (left) {
        const Int a = py::RFind(msl, "\n", 0, static_cast<Int>(left->start())) + 1;
        const std::string line = py::Split(py::Slice(msl, a, static_cast<Int>(left->end()) + 100), "\n")[0];
        throw py::ValueError(
            absl::StrCat("a bounded uniform read is not written as a select: ", py::To(py::Strip(line), 160)));
    }
    return msl;
}

Json FragPos(const std::string& msl, std::string_view var, std::string_view comp) {
    auto one = [&](std::string_view n, std::string_view pat) -> std::optional<re::Match> {
        auto m = re::search(absl::StrFormat("float %s = %s;", re::escape(n), pat), msl);
        if (m && Once(msl, n)) return m;
        return std::nullopt;
    };
    auto pixel = [&](std::string_view n) {
        if (one(n, absl::StrFormat(R"(gl_FragCoord\.%s)", comp))) return true;
        auto q = one(n, R"((_\d+) / (_\d+))");
        return q && one(q->group(1), absl::StrFormat(R"(gl_FragCoord\.%s)", comp)) &&
               one(q->group(2), re::escape(kScale));
    };
    auto m = one(var, R"((\S+) \* (\S+))");
    if (!m) return Json();
    static const re::Pattern uni(R"(\(\*spvDescriptorSet0\.fp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw]))");
    static const re::Pattern tmp(R"(_\d+)");
    const std::string g1(m->group(1)), g2(m->group(2));
    for (const auto& [a, u] : {std::pair{g1, g2}, std::pair{g2, g1}}) {
        auto k = uni.fullmatch(u);
        if (k && py::ToInt(k->group(1)) >= 3 && tmp.fullmatch(a) && pixel(a)) {
            Json out = Json::MakeArray();
            out.push_back(py::ToInt(k->group(1)) - 3);
            out.push_back(py::ToInt(k->group(2)));
            out.push_back(static_cast<int>(std::string_view("xyzw").find(k->group(3))));
            return out;
        }
    }
    return Json();
}

std::vector<int> EntryScreen(const std::string& body, const std::map<std::string, int>& locn) {
    static const re::Pattern ret(R"(\breturn\b)");
    static const re::Pattern ret_end(R"(\n    return out;\n$)");
    if (ret.finditer(body).size() != 1 || !ret_end.search(body + "\n")) return {};
    std::vector<std::string> lines;
    for (const std::string& ln : py::Split(body, "\n")) lines.emplace_back(py::Strip(ln));
    static const re::Pattern control(R"((if|else|for|while|do|switch)\b)");
    for (std::size_t n = 0; n < lines.size(); ++n) {
        if (control.match(lines[n]) && (n + 1 >= lines.size() || lines[n + 1] != "{")) return {};
    }
    int depth = 0;
    std::vector<std::pair<int, std::string>> stmts;
    for (const std::string& s : lines) {
        if (s == "{") {
            ++depth;
            continue;
        }
        if (s == "}") {
            --depth;
            continue;
        }
        if (!s.empty()) stmts.emplace_back(depth, s);
    }
    struct At {
        std::size_t n;
        int depth;
        std::optional<std::string> expr;
    };
    std::map<std::string, At> decl;
    std::map<std::pair<std::string, std::string>, At> last;
    std::map<std::string, int> nassign;
    static const re::Pattern assign(R"((?<![\w.])(_\d+) = )");
    for (const std::string& v : assign.findall(body)) ++nassign[v];
    static const re::Pattern decl_p(R"((?:float|int|uint|bool|half) (_\d+) = (.+);)");
    static const re::Pattern comp_p(R"(out\.(gl_Position|m_\d+)\.([xyzw]) = (.+);)");
    static const re::Pattern vec_p(R"(out\.(gl_Position|m_\d+) = float4\((.+)\);)");
    for (std::size_t n = 0; n < stmts.size(); ++n) {
        const auto& [dp, s] = stmts[n];
        if (auto m = decl_p.fullmatch(s)) {
            decl[std::string(m->group(1))] = {n, dp, std::string(m->group(2))};
            continue;
        }
        if (auto m = comp_p.fullmatch(s)) {
            last[{std::string(m->group(1)), std::string(m->group(2))}] = {n, dp, std::string(m->group(3))};
            continue;
        }
        if (auto m = vec_p.fullmatch(s)) {
            std::vector<std::string> parts;
            for (const std::string& p : py::Split(m->group(2), ",")) parts.emplace_back(py::Strip(p));
            const char* cs = "xyzw";
            for (int c = 0; c < 4; ++c) {
                last[{std::string(m->group(1)), std::string(1, cs[c])}] =
                    parts.size() == 4 ? At{n, dp, parts[c]} : At{n, dp, std::nullopt};
            }
        }
    }
    std::map<std::string, At> pos;
    for (const char* c : {"x", "y", "w"}) {
        auto p = last.find({"gl_Position", c});
        if (p == last.end() || p->second.depth != 0 || !p->second.expr) return {};
        pos[c] = p->second;
    }
    static const re::Pattern tmp(R"(_\d+)");
    std::map<std::string, std::string> sym;
    for (const char* c : {"x", "y", "w"}) {  // Python's dict order: a later component wins a variable they share
        const std::optional<std::string>& e = pos[c].expr;
        if (e && tmp.fullmatch(*e)) sym[*e] = absl::StrCat("P", c);
    }
    std::function<Env(std::size_t)> env_at = [&](std::size_t at) -> Env {
        auto memo = std::make_shared<std::map<std::string, std::optional<Form>>>();
        return [&, at, memo](const std::string& name) -> std::optional<Form> {
            if (py::StartsWith(name, "out.gl_Position.")) {
                const std::string c = name.substr(name.size() - 1);
                auto p = pos.find(c);
                if (p != pos.end() && p->second.n < at) return Form{{"P" + c, 1.0}};
                return std::nullopt;
            }
            if (auto s = sym.find(name); s != sym.end()) return Form{{s->second, 1.0}};
            if (auto it = memo->find(name); it != memo->end()) return it->second;
            auto d = decl.find(name);
            if (d == decl.end() || d->second.n >= at || nassign[name] != 1) return std::nullopt;
            (*memo)[name] = std::nullopt;
            std::optional<Form> v = LinOf(*d->second.expr, env_at(d->second.n));
            (*memo)[name] = v;
            return v;
        };
    };
    const std::pair<const char*, Form> want[] = {
        {"x", {{"Px", 0.5}, {"Pw", 0.5}}}, {"y", {{"Py", -0.5}, {"Pw", 0.5}}}, {"w", {{"Pw", 1.0}}}};
    std::set<int> got;
    for (const auto& [mem, k] : locn) {
        bool ok = true;
        for (const auto& [c, w] : want) {
            auto a = last.find({mem, c});
            const std::optional<std::string>* e = a == last.end() ? nullptr : &a->second.expr;
            if (!e || a->second.depth != 0 || !*e) {
                ok = false;
                break;
            }
            const std::optional<Form> f = LinOf(**e, env_at(a->second.n));
            if (!f || *f != w) {
                ok = false;
                break;
            }
        }
        if (ok) got.insert(k);
    }
    return {got.begin(), got.end()};
}

std::vector<int> ScreenVaryings(const std::string& msl) {
    static const re::Pattern ost_p(R"(struct main0_out\s*\{(.*?)\};)", re::S);
    static const re::Pattern locn_p(R"(float4 (m_\d+) \[\[user\(locn(\d+)\)\]\])");
    static const re::Pattern entries_p(R"(\nvertex main0_out \w+\(.*?\)\n\{\n(.*?)\n\}\n)", re::S);
    auto ost = ost_p.search(msl);
    std::map<std::string, int> locn;
    for (const re::Match& m : locn_p.finditer(ost ? ost->group(1) : std::string_view()))
        locn[std::string(m.group(1))] = static_cast<int>(py::ToInt(m.group(2)));
    const std::vector<re::Match> entries = entries_p.finditer(msl);
    if (locn.empty() || entries.empty()) return {};
    std::set<int> got;
    for (const auto& [m, k] : locn) got.insert(k);
    for (const re::Match& e : entries) {
        const std::vector<int> s = EntryScreen(std::string(e.group(1)), locn);
        const std::set<int> ss(s.begin(), s.end());
        std::set<int> both;
        for (int k : got)
            if (ss.count(k)) both.insert(k);
        got = both;
    }
    return {got.begin(), got.end()};
}

Fold MakeFold(const std::string& msl, const Json& bindings, bool om_only_ok) {
    auto why = [](std::string_view w) { return Fold{std::nullopt, Json(w)}; };
    const std::optional<std::string> shared = side::SharedBody(msl);
    if (!shared) return why("main function signature not recognized");
    const std::string& src = *shared;
    std::optional<side::BodySpan> where;
    std::optional<side::Entry> e;
    try {
        where = side::Body(src);
        e = side::FindEntry(src, "main0");
    } catch (const py::ValueError& x) {
        return why(x.what());
    }
    if (!where || !e) return why("main function signature not recognized");
    const Int start = where->start, head = where->head, end = where->end;
    const std::string text(py::Slice(src, head, end));
    static const re::Pattern tex_p(R"(texture2d<float> (\w+) \[\[id\(\d+\)\]\];)");
    std::set<std::string> tex;
    for (const std::string& t : tex_p.findall(src)) tex.insert(t);
    std::map<std::string, int> locn;
    {
        const std::vector<std::string> a = py::Split(src, "struct main0_in", 1);
        const std::string in = py::Split(a.back(), "};", 1)[0];
        static const re::Pattern locn_p(R"(float4 (m_\d+) \[\[user\(locn(\d+)\)\]\];)");
        for (const re::Match& m : locn_p.finditer(in))
            locn[std::string(m.group(1))] = static_cast<int>(py::ToInt(m.group(2)));
    }
    std::map<std::string, Int> reg;
    std::vector<std::string> reg_order;
    for (const Json& b : bindings.array()) {
        const Json* kind = b.find("kind");
        const Json* name = b.find("msl_name");
        const Json* r = b.find("reg");
        if (kind && kind->is_string() && kind->as_string() == abi::kKindTex && name && name->truthy() && r &&
            !r->is_null()) {
            if (!reg.count(name->as_string())) reg_order.push_back(name->as_string());
            reg[name->as_string()] = r->as_int();
        }
    }
    static const re::Pattern pat(absl::StrFormat(
        R"((dm_bs\()?spvDescriptorSet0\.(\w+)\.sample\(spvDescriptorSet0\.(\w+)Smplr, select\((_\d+), float2\(0\.0\), isnan\(\4\)\)\))"
        R"((?(1), spvDescriptorSet0\.%s, \d+u, spvDescriptorSet0\.\2, spvDescriptorSet0\.\3Smplr%s, select\(\4, float2\(0\.0\), isnan\(\4\)\)\)))",
        abi::kBorderMember, abi::kBorderTwin));
    Json calls = Json::MakeArray();
    std::string out_parts;
    std::size_t pos = 0;
    std::map<Int, int> conv;
    for (const re::Match& m : pat.finditer(text)) {
        const std::string member(m.group(2)), var(m.group(4));
        if (!tex.count(member) || member != m.group(3) || !reg.count(member)) continue;
        auto d = re::search(absl::StrFormat(R"(float2 %s = float2\((_\d+), (_\d+)\);)", var), text);
        std::optional<re::Match> a, b;
        if (d) {
            a = re::search(absl::StrFormat(R"(float %s = in\.(m_\d+)\.x;)", d->group(1)), text);
            b = re::search(absl::StrFormat(R"(float %s = in\.(m_\d+)\.y;)", d->group(2)), text);
        }
        Json how = Json::MakeObject();
        if (a && b && a->group(1) == b->group(1) && locn.count(std::string(a->group(1)))) {
            how.set("varying_locn", locn[std::string(a->group(1))]);
        } else {
            std::optional<std::string> pa, pb;
            Json fx, fy;
            if (d) {
                pa = Proj(text, d->group(1), "x", locn);
                pb = Proj(text, d->group(2), "y", locn);
                fx = FragPos(text, d->group(1), "x");
                fy = FragPos(text, d->group(2), "y");
            }
            if (pa && pa == pb) {
                how.set("varying_locn", locn[*pa]);
                how.set("proj", 1);
            } else if (fx.truthy() && fy.truthy()) {
                Json both = Json::MakeArray();
                both.push_back(fx);
                both.push_back(fy);
                how.set("frag_pos", both);
            } else {
                continue;
            }
        }
        const int i = static_cast<int>(calls.size());
        if (i >= 32) break;
        std::string sel;
        for (int k = 0; k < kNfb; ++k) sel += absl::StrFormat("fx%d == %d ? fb%d : ", i, k, k);
        out_parts += text.substr(pos, m.start() - pos);
        out_parts += absl::StrCat("(", sel, m.group(0), ")");
        pos = m.end();
        Json c = Json::MakeObject();
        c.set("fc", i);
        c.set("tex_reg", reg[member]);
        c.update(how);
        ++conv[reg[member]];
        calls.push_back(c);
    }
    if (calls.size() == 0 && !om_only_ok) return why("no rewritable sample");
    std::set<Int> mixed;
    for (const std::string& mb : reg_order) {
        if (tex.count(mb) && py::Count(text, absl::StrCat("spvDescriptorSet0.", mb, ".")) > conv[reg[mb]])
            mixed.insert(reg[mb]);
    }
    static const re::Pattern om_p(R"(struct main0_out\s*\{(.*?)\};)", re::S);
    auto om = om_p.search(src);
    if (!om) return why("no output struct");
    static const re::Pattern out_p(R"((\w+) (m_\d+) \[\[color\((\d+)\)\]\];)");
    struct Out {
        std::string t, n;
        Int c;
    };
    std::vector<Out> outs;
    for (const re::Match& m : out_p.finditer(om->group(1)))
        outs.push_back({std::string(m.group(1)), std::string(m.group(2)), py::ToInt(m.group(3))});
    static const re::Pattern plain(R"(\w+ m_\d+ \[\[color\(\d+\)\]\];)");
    if (outs.empty() || outs.size() > 4 || !py::Strip(plain.sub("", om->group(1))).empty())
        return why("the output struct is not plain color outputs (or has more than 4)");
    std::string rest = src.substr(end);
    std::string sig;
    if (calls.size()) {
        std::vector<std::string> params, off;
        for (std::size_t i = 0; i < calls.size(); ++i) params.push_back(absl::StrCat("int fx", i));
        for (int k = 0; k < kNfb; ++k) params.push_back(absl::StrCat("float4 fb", k));
        for (std::size_t i = 0; i < calls.size(); ++i) off.emplace_back("-1");
        for (int k = 0; k < kNfb; ++k) off.emplace_back("float4(0.0)");
        const std::string offs = py::Join(off, ", ");
        sig = std::string(py::Slice(src, start, head - 1));
        sig += (py::EndsWith(sig, "(") ? "" : ", ") + py::Join(params, ", ") + ")";
        static const re::Pattern call(R"(\bdm_body_main0\()");
        std::string parts;
        Int at = 0;
        for (const re::Match& m : call.finditer(rest)) {
            const Int close = side::Close(rest, static_cast<Int>(m.end()), "(", ")") - 1;
            parts += py::Slice(rest, at, close);
            parts += (py::Strip(py::Slice(rest, static_cast<Int>(m.end()), close)).empty() ? "" : ", ") + offs;
            at = close;
        }
        rest = parts + std::string(py::From(rest, at));
    } else {
        sig = std::string(py::Slice(src, start, head));
    }
    std::string consts;
    for (const Json& c : calls.array())
        consts += absl::StrFormat("constant int FX_%d [[function_constant(%d)]];\n", c.at("fc").as_int(),
                                  c.at("fc").as_int());
    for (int k = 0; k < kNfb; ++k)
        consts += absl::StrFormat("constant bool FB_%d [[function_constant(%d)]];\n", k, 32 + k);
    for (std::size_t j = 0; j < outs.size(); ++j)
        for (int k = 0; k < kNfb; ++k)
            consts += absl::StrFormat("constant bool OM_%d_%d [[function_constant(%d)]];\n", j, k, 64 + 8 * j + k);
    std::string lead = src.substr(0, start);
    if (!py::Contains(lead, "using namespace metal;\n")) return why("no using namespace metal");
    lead = py::Replace(lead, "using namespace metal;\n", "using namespace metal;\n\n" + consts, 1);
    std::string members;
    for (std::size_t j = 0; j < outs.size(); ++j)
        for (int k = 0; k < kNfb; ++k)
            members += absl::StrFormat("    %s %s_at%d [[color(%d), function_constant(OM_%d_%d)]];\n", outs[j].t,
                                       outs[j].n, k, k, j, k);
    std::vector<std::string> fbs;
    fbs.reserve(kNfb);
    for (int k = 0; k < kNfb; ++k)
        fbs.push_back(absl::StrFormat("float4 fb%d [[color(%d), function_constant(FB_%d)]]", k, k, k));
    std::vector<std::string> args;
    if (!e->args.empty()) args.push_back(e->args);
    if (calls.size()) {
        for (std::size_t i = 0; i < calls.size(); ++i) args.push_back(absl::StrCat("FX_", i));
        for (int k = 0; k < kNfb; ++k) args.push_back(absl::StrCat("_fb", k));
    }
    std::string load;
    if (calls.size())
        for (int k = 0; k < kNfb; ++k)
            load += absl::StrFormat("    float4 _fb%d = float4(0.0);\n    if (FB_%d) _fb%d = fb%d;\n", k, k, k, k);
    std::string put;
    for (std::size_t j = 0; j < outs.size(); ++j)
        for (int k = 0; k < kNfb; ++k)
            put += absl::StrFormat("    if (OM_%d_%d) out.%s_at%d = r.%s;\n", j, k, outs[j].n, k, outs[j].n);
    std::vector<std::string> fparams = e->params;
    fparams.insert(fparams.end(), fbs.begin(), fbs.end());
    const std::string main0_f =
        absl::StrCat("\nstruct main0_out_f\n{\n", members, "};\n\n", e->attr, "fragment main0_out_f main0_f(",
                     py::Join(fparams, ", "), ")\n{\n", load, "    main0_out r = dm_body_main0(", py::Join(args, ", "),
                     ");\n    main0_out_f out = {};\n", put, "    return out;\n}\n");
    Json meta = Json::MakeObject();
    meta.set("calls", calls);
    Json mx = Json::MakeArray();
    for (Int r : mixed) mx.push_back(r);
    meta.set("mixed_regs", mx);
    Json oj = Json::MakeArray();
    for (const Out& o : outs) {
        Json x = Json::MakeObject();
        x.set("color", o.c);
        x.set("type", o.t);
        oj.push_back(x);
    }
    meta.set("outputs", oj);
    return Fold{absl::StrCat(lead, sig, out_parts, text.substr(pos), rest, main0_f), meta};
}

}  // namespace nxmac::dmsh::fetch
