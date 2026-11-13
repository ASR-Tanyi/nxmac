// dmsh_side.py, dmsh_alpha.py, dmsh_logic.py: a pixel shader's alpha test and logic operation entry points in one
// compile unit, and the cut of its library into the base, side and readable-tile libraries.
#include <cstring>
#include <memory>

#include "abi.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "modules.h"
#include "re.h"

namespace nxmac::dmsh {
namespace {

using py::Int;

const re::Pattern& EntryPattern() {
    static const re::Pattern p(R"(^((?:\[\[[^\]]*\]\]\s*)?)fragment\s+(\w+)\s+(\w+)\s*\()", re::M);
    return p;
}

const re::Pattern& BodyPattern() {
    static const re::Pattern p(R"(static (__attribute__\(\(always_inline\)\) )?main0_out dm_body_main0\()");
    return p;
}

side::Entry MakeEntry(const std::string& msl, const re::Match& m) {
    side::Entry e;
    e.attr = m.group(1);
    e.ret = m.group(2);
    e.name = m.group(3);
    const Int head = side::Close(msl, static_cast<Int>(m.end()), "(", ")");
    const Int body = head >= 0 ? py::Find(msl, "{", head) : -1;
    const Int end = body >= 0 ? side::Close(msl, body + 1, "{", "}") : -1;
    if (end < 0) throw py::ValueError(absl::StrCat("no body for ", e.name));
    e.params = side::Params(py::Slice(msl, static_cast<Int>(m.end()), head - 1));
    static const re::Pattern attr(R"(\s*\[\[.*\]\]$)");
    static const re::Pattern last(R"((\w+)$)");
    std::vector<std::string> decl, args;
    for (const std::string& p : e.params) {
        const std::string d = attr.sub("", p);
        auto a = last.search(d);
        if (!a || py::Contains(p, "function_constant"))
            throw py::ValueError(absl::StrCat("a parameter of ", e.name, ": ", py::To(p, 80)));
        decl.push_back(d);
        args.emplace_back(a->group(1));
    }
    e.decl = py::Join(decl, ", ");
    e.args = py::Join(args, ", ");
    e.start = static_cast<Int>(m.start());
    e.head = head;
    e.end = end;
    return e;
}

std::uint32_t U32(const std::string& b, std::size_t p) {
    if (p + 4 > b.size())
        throw py::ValueError(
            absl::StrFormat("an unreadable library (unpack_from requires a buffer of at least %d bytes for unpacking 4 "
                            "bytes at offset %d (actual buffer size is %d))",
                            p + 4, p, b.size()));
    std::uint32_t v;
    std::memcpy(&v, b.data() + p, 4);
    return v;
}
std::uint16_t U16(const std::string& b, std::size_t p) {
    if (p + 2 > b.size())
        throw py::ValueError(
            absl::StrFormat("an unreadable library (unpack_from requires a buffer of at least %d bytes for unpacking 2 "
                            "bytes at offset %d (actual buffer size is %d))",
                            p + 2, p, b.size()));
    std::uint16_t v;
    std::memcpy(&v, b.data() + p, 2);
    return v;
}
std::uint64_t U64(const std::string& b, std::size_t p) {
    if (p + 8 > b.size())
        throw py::ValueError(
            absl::StrFormat("an unreadable library (unpack_from requires a buffer of at least %d bytes for unpacking 8 "
                            "bytes at offset %d (actual buffer size is %d))",
                            p + 8, p, b.size()));
    std::uint64_t v;
    std::memcpy(&v, b.data() + p, 8);
    return v;
}
void Put64(std::string& b, std::size_t p, std::uint64_t v) { std::memcpy(b.data() + p, &v, 8); }
std::string Pack16(std::uint16_t v) { return std::string(reinterpret_cast<const char*>(&v), 2); }
std::string Pack32(std::uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); }
std::string Pack64(std::uint64_t v) { return std::string(reinterpret_cast<const char*>(&v), 8); }
std::string Bytes(const std::string& b, std::size_t a, std::size_t e) {  // b[a:e]
    if (a >= b.size() || e <= a) return {};
    return b.substr(a, std::min(e, b.size()) - a);
}

using Tags = std::vector<std::pair<std::string, std::string>>;

Tags ReadTags(const std::string& b, std::size_t p, std::size_t end) {
    Tags out;
    while (p + 4 <= end) {
        const std::string tag = Bytes(b, p, p + 4);
        p += 4;
        if (tag == "ENDT") return out;
        const std::size_t n = U16(b, p);
        p += 2;
        out.emplace_back(tag, Bytes(b, p, p + n));
        p += n;
    }
    throw py::ValueError("an unterminated tag list");
}

const std::string& Tag(const Tags& t, std::string_view name) {  // dict(t)[name]: the last of that tag
    for (auto it = t.rbegin(); it != t.rend(); ++it)
        if (it->first == name) return it->second;
    throw py::ValueError(absl::StrCat("an unreadable library (b'", name, "')"));
}

std::vector<std::string> Sized(const std::string& b, std::size_t p, std::size_t n) {
    std::vector<std::string> out;
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t size = U32(b, p);
        out.push_back(Bytes(b, p, p + 4 + size));
        p += 4 + size;
    }
    return out;
}

std::string Take(const std::string& b, const std::vector<std::size_t>& keep, const std::vector<Tags>& fns,
                 const std::string& tail, const std::uint64_t (&sections)[8]) {
    const std::uint64_t fo = sections[0], fs = sections[1], po = sections[2], qo = sections[4], bo = sections[6];
    const std::vector<std::string> pub = Sized(b, po, fns.size()), priv = Sized(b, qo, fns.size());
    std::string entries, newpub, newpriv, code;
    for (std::size_t i : keep) {
        const std::string& offt = Tag(fns[i], "OFFT");
        const std::string& mdsz = Tag(fns[i], "MDSZ");
        if (offt.size() != 24) throw py::ValueError("an unreadable library (unpack requires a buffer of 24 bytes)");
        if (mdsz.size() != 8) throw py::ValueError("an unreadable library (unpack requires a buffer of 8 bytes)");
        const std::uint64_t code_off = U64(offt, 16), size = U64(mdsz, 0);
        std::string body;
        for (const auto& [tag, v] : fns[i]) {
            if (tag == "OFFT") {
                body += tag + Pack16(24) + Pack64(newpub.size()) + Pack64(newpriv.size()) + Pack64(code.size());
            } else {
                absl::StrAppend(&body, tag, Pack16(static_cast<std::uint16_t>(v.size())), v);
            }
        }
        body += "ENDT";
        entries += Pack32(static_cast<std::uint32_t>(body.size() + 4)) + body;
        newpub += pub[i];
        newpriv += priv[i];
        code += Bytes(b, bo + code_off, bo + code_off + size);
    }
    const std::string listing = Pack32(static_cast<std::uint32_t>(keep.size())) + entries;
    const __int128 all_entries = static_cast<__int128>(po) - tail.size() - fo - 4;
    const std::uint64_t nfo = fo, npo = fo + listing.size() + tail.size(), nqo = npo + newpub.size(),
                        nbo = nqo + newpriv.size();
    const __int128 nfs = static_cast<__int128>(fs) - all_entries + listing.size() - 4;
    if (nfs < 0 || fo > b.size() || fo < 0x58) throw py::ValueError("an unreadable library (argument out of range)");
    std::string head = b.substr(0, fo);
    Put64(head, 0x10, nbo + code.size());
    const std::uint64_t values[8] = {
        nfo, static_cast<std::uint64_t>(nfs), npo, newpub.size(), nqo, newpriv.size(), nbo, code.size()};
    for (int k = 0; k < 8; ++k) Put64(head, 0x18 + 8 * k, values[k]);
    return head + listing + tail + newpub + newpriv + code;
}

}  // namespace

namespace side {

Int Close(std::string_view src, Int i, std::string_view open, std::string_view close) {
    int depth = 1;
    static const re::Pattern parens(R"(\(|\))");
    static const re::Pattern braces(R"(\{|\})");
    std::unique_ptr<re::Pattern> other;
    if (open != "(" && open != "{")
        other = std::make_unique<re::Pattern>(absl::StrCat(re::escape(open), "|", re::escape(close)));
    const re::Pattern& use = open == "(" ? parens : open == "{" ? braces : *other;
    for (const re::Match& m : use.finditer(src, static_cast<std::size_t>(std::max<Int>(i, 0)))) {
        depth += m.group() == open ? 1 : -1;
        if (!depth) return static_cast<Int>(m.end());
    }
    return -1;
}

std::vector<std::string> Params(std::string_view s) {
    std::vector<std::string> out;
    std::size_t at = 0;
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        depth += (ch == '<' || ch == '(' || ch == '[') - (ch == '>' || ch == ')' || ch == ']');
        if (ch == ',' && !depth) {
            out.emplace_back(py::Strip(s.substr(at, i - at)));
            at = i + 1;
        }
    }
    const std::string_view last = py::Strip(s.substr(std::min(at, s.size())));
    if (!last.empty()) out.emplace_back(last);
    return out;
}

std::vector<Entry> Entries(const std::string& msl) {
    std::vector<Entry> out;
    for (const re::Match& m : EntryPattern().finditer(msl)) out.push_back(MakeEntry(msl, m));
    return out;
}

std::optional<Entry> FindEntry(const std::string& msl, std::string_view name) {
    for (const re::Match& m : EntryPattern().finditer(msl))
        if (m.group(3) == name) return MakeEntry(msl, m);
    return std::nullopt;
}

std::optional<BodySpan> Body(const std::string& msl) {
    auto m = BodyPattern().search(msl);
    if (!m) return std::nullopt;
    const Int head = Close(msl, static_cast<Int>(m->end()), "(", ")");
    const Int end = Close(msl, py::Find(msl, "{", head) + 1, "{", "}");
    if (head >= 0 && end >= 0) return BodySpan{static_cast<Int>(m->start()), head, end};
    return std::nullopt;
}

std::optional<std::string> SharedBody(const std::string& msl) {
    if (auto m = BodyPattern().search(msl)) {
        if (m->has(1)) return msl;
        return msl.substr(0, m->start()) + std::string(kBody) + msl.substr(m->end());
    }
    std::optional<Entry> e;
    try {
        e = FindEntry(msl, "main0");
    } catch (const py::ValueError&) {
        return std::nullopt;
    }
    if (!e || e->ret != "main0_out") return std::nullopt;
    return absl::StrCat(msl.substr(0, e->start), kBody, e->decl, ")", msl.substr(e->head, e->end - e->head), "\n",
                        Calling(*e, "main0", "main0_out", "return out;"), msl.substr(e->end));
}

std::string Calling(const Entry& e, std::string_view name, std::string_view ret, std::string_view tail) {
    return absl::StrCat(e.attr, "fragment ", ret, " ", name, "(", py::Join(e.params, ", "),
                        ")\n{\n    main0_out out = dm_body_main0(", e.args, ");\n    ", tail, "\n}\n");
}

Unit MakeUnit(const std::string& msl) {
    Unit u;
    std::vector<Entry> es;
    try {
        es = Entries(msl);
    } catch (const py::ValueError& e) {
        u.why = e.what();
        return u;
    }
    if (es.empty() || es[0].name != "main0") {
        u.why = "no fragment entry point main0";
        return u;
    }
    std::optional<std::string> no_alpha = alpha::Refused(msl), no_logic = logic::Refused(msl);
    std::vector<std::pair<std::string, std::optional<std::string>>> alpha;
    if (!no_alpha) {
        for (const Entry& e : es) alpha.emplace_back(e.name + "_a", alpha::EntryText(msl, e));
    }
    bool all = true;
    for (const auto& [n, a] : alpha) all = all && a.has_value() && !a->empty();
    if (!all) {
        alpha.clear();
        no_alpha = "an entry point without a colour 0";
    }
    std::vector<std::pair<std::string, std::pair<std::string, std::set<int>>>> logic;
    for (const Entry& e : es) {
        if (no_logic) continue;
        if (auto o = logic::EntryText(msl, e)) logic.emplace_back(e.name + "_l", *o);
    }
    if (alpha.empty() && logic.empty()) {
        u.why = no_alpha ? *no_alpha : no_logic ? *no_logic : "no colour output";
        return u;
    }
    std::vector<std::string> thin;
    thin.reserve(alpha.size() + logic.size());
    for (const auto& [n, a] : alpha) thin.push_back(a.value_or(""));
    std::set<int> targets;
    for (const auto& [n, o] : logic) {
        thin.push_back(o.first);
        targets.insert(o.second.begin(), o.second.end());
    }
    std::string src;
    Int at = es[0].start;
    const bool shared = BodyPattern().search(msl).has_value();
    for (const Entry& e : es) {
        src += py::Slice(msl, at, e.start);
        if (!(shared && e.name == "main0")) {
            src += absl::StrCat("static ", e.ret, " dm_body_", e.name, "(", e.decl, ")");
            src += py::Slice(msl, e.head, e.end);
        }
        at = e.end;
    }
    std::string plain;
    for (const Entry& e : es) {
        plain += absl::StrCat(e.attr, "fragment ", e.ret, " ", e.name, "(", py::Join(e.params, ", "),
                              ")\n{\n    return dm_body_", e.name, "(", e.args, ");\n}\n");
    }
    u.src = absl::StrCat(py::To(msl, es[0].start), alpha::kHelper, logic::kHelper, logic::Constants(targets), src,
                         py::From(msl, at), "\n", plain, py::Join(thin, ""));
    for (const auto& [n, a] : alpha) u.alpha.push_back(n);
    for (const auto& [n, o] : logic) u.logic.push_back(n);
    return u;
}

std::vector<std::string> Cut(const std::string& lib, const std::vector<std::set<std::string>>& groups) {
    if (lib.size() < 4 || lib.substr(0, 4) != "MTLB" || U64(lib, 0x10) != lib.size())
        throw py::ValueError("not a whole Metal library");
    std::uint64_t sections[8];
    for (int k = 0; k < 8; ++k) sections[k] = U64(lib, 0x18 + 8 * k);
    const std::uint64_t fo = sections[0], po = sections[2], bo = sections[6], bs = sections[7];
    if (bo + bs != lib.size()) throw py::ValueError("its bitcode is not last");
    const std::size_t n = U32(lib, fo);
    std::size_t p = fo + 4;
    std::vector<Tags> fns;
    std::vector<std::string> names;
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t size = U32(lib, p);
        Tags t = ReadTags(lib, p + 4, p + size);
        std::string name = Tag(t, "NAME");
        while (!name.empty() && name.back() == '\0') name.pop_back();
        names.push_back(name);
        fns.push_back(std::move(t));
        p += size;
    }
    std::set<std::string> named;
    std::size_t total = 0;
    for (const auto& g : groups) {
        named.insert(g.begin(), g.end());
        total += g.size();
    }
    const std::set<std::string> have(names.begin(), names.end());
    for (const std::string& x : named)
        if (!have.count(x)) throw py::ValueError(absl::StrCat("no function ", x));
    if (total != named.size()) throw py::ValueError("a function in two groups");
    const std::string tail = Bytes(lib, p, po);
    std::vector<std::string> out;
    for (std::size_t g = 0; g <= groups.size(); ++g) {
        std::vector<std::size_t> keep;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (g == 0 ? !named.count(names[i]) : groups[g - 1].count(names[i]) > 0) keep.push_back(i);
        }
        out.push_back(Take(lib, keep, fns, tail, sections));
    }
    return out;
}

}  // namespace side

namespace alpha {

const std::string_view kHelper = R"(
struct dm_alpha_state { uint func; float ref; };   // NVN_ALPHA_FUNC_* and the reference (dmsh_alpha.py)
static inline bool dm_alpha_pass(float a, constant dm_alpha_state& s)
{
    switch (s.func)
    {
        case 1u: return false;
        case 2u: return a < s.ref;
        case 3u: return a == s.ref;
        case 4u: return a <= s.ref;
        case 5u: return a > s.ref;
        case 6u: return a != s.ref;
        case 7u: return a >= s.ref;
        default: return true;
    }
}
)";

std::pair<std::string, std::string> Colour0(const std::string& msl, std::string_view st_name) {
    const re::Pattern st_p(absl::StrFormat(R"(struct %s\s*\{(.*?)\};)", st_name), re::S);
    static const re::Pattern col_p(R"(^\s*(float4|half4|uint4)\s+(\w+)\s*\[\[color\(0\)(?:, index\(0\))?\]\];)", re::M);
    auto st = st_p.search(msl);
    if (!st) return {};
    auto col = col_p.search(st->group(1));
    if (!col) return {};
    return {std::string(col->group(1)), std::string(col->group(2))};
}

std::optional<std::string> Refused(const std::string& msl) {
    static const re::Pattern st_p(R"(struct main0_out\s*\{(.*?)\};)", re::S);
    auto st = st_p.search(msl);
    if (!st) return "no colour output";
    const std::string ty = Colour0(msl, "main0_out").first;
    if (ty.empty() || ty == "uint4") {
        return re::search(R"(\[\[color\(0\)\]\])", st->group(1)) ? "colour 0 has no float alpha" : "no colour 0 output";
    }
    if (py::Contains(msl, absl::StrCat("[[buffer(", abi::kAlphaBuffer, ")]]")))
        return absl::StrCat("fragment buffer ", abi::kAlphaBuffer, " is taken");
    return std::nullopt;
}

std::optional<std::string> EntryText(const std::string& msl, const side::Entry& e) {
    const auto [ty, name] = Colour0(msl, e.ret);
    if (ty.empty()) return std::nullopt;
    const std::string a =
        ty == "uint4" ? absl::StrCat("as_type<float>(dm_r.", name, ".w)") : absl::StrCat("float(dm_r.", name, ".w)");
    std::vector<std::string> params = e.params;
    params.push_back(absl::StrCat("constant dm_alpha_state& dm_alpha [[buffer(", abi::kAlphaBuffer, ")]]"));
    return absl::StrCat(e.attr, "fragment ", e.ret, " ", e.name, "_a(", py::Join(params, ", "), ")\n{\n    ", e.ret,
                        " dm_r = dm_body_", e.name, "(", e.args, ");\n", "    if (!dm_alpha_pass(", a,
                        ", dm_alpha)) discard_fragment();\n    return dm_r;\n}\n");
}

}  // namespace alpha

namespace logic {

const std::string_view kHelper = R"(
struct dm_logic_state { uint op; uint pad0, pad1, pad2; uint4 mask[8]; };   // NVN_LOGIC_OP_* and each colour target's channel masks (dmsh_logic.py)
static inline uint4 dm_logic(uint4 s, uint4 d, constant dm_logic_state& l, uint n)
{
    uint4 r;
    switch (l.op)
    {
        case 0u: r = uint4(0u); break;
        case 1u: r = s & d; break;
        case 2u: r = s & ~d; break;
        case 4u: r = ~s & d; break;
        case 5u: r = d; break;
        case 6u: r = s ^ d; break;
        case 7u: r = s | d; break;
        case 8u: r = ~(s | d); break;
        case 9u: r = ~(s ^ d); break;
        case 10u: r = ~d; break;
        case 11u: r = s | ~d; break;
        case 12u: r = ~s; break;
        case 13u: r = ~s | d; break;
        case 14u: r = ~(s & d); break;
        case 15u: r = uint4(~0u); break;
        default: r = s; break;
    }
    return r & l.mask[n];
}
static inline float4 dm_logic_norm(float4 s, float4 d, constant dm_logic_state& l, uint n)
{
    if (all(l.mask[n] == uint4(0u))) return s;   // not a normalized target: the operation does not apply
    float4 scale = float4(l.mask[n]);
    return float4(dm_logic(uint4(floor(saturate(s) * scale + 0.5f)), uint4(floor(saturate(d) * scale + 0.5f)), l, n)) / scale;
}
)";

std::optional<std::string> Refused(const std::string& msl) {
    if (py::Contains(msl, absl::StrCat("[[buffer(", abi::kLogicBuffer, ")]]")))
        return absl::StrCat("fragment buffer ", abi::kLogicBuffer, " is taken");
    static const re::Pattern p(R"(\[\[color\(\d+\)\]\]\s*[,)])");
    if (p.search(msl)) return "an entry point already reads an attachment";
    return std::nullopt;
}

std::string Constants(const std::set<int>& targets) {
    std::string out;
    for (int n : targets)
        out += absl::StrFormat("constant bool dm_lfb%d [[function_constant(%d)]];\n", n, abi::kLogicFc + n);
    return out;
}

std::optional<std::pair<std::string, std::set<int>>> EntryText(const std::string& msl, const side::Entry& e) {
    if (e.name != "main0" && e.name != "main0_u" && e.name != "main0_m") return std::nullopt;
    const re::Pattern st_p(absl::StrFormat(R"(struct %s\s*\{(.*?)\};)", e.ret), re::S);
    static const re::Pattern out_p(R"(^\s*((?:uint|float)[234]?)\s+(\w+)\s*\[\[color\((\d+)\)\]\];)", re::M);
    auto st = st_p.search(msl);
    struct Out {
        std::string ty, nm;
        int n;
    };
    std::vector<Out> outs;
    if (st) {
        for (const re::Match& m : out_p.finditer(st->group(1)))
            outs.push_back({std::string(m.group(1)), std::string(m.group(2)), static_cast<int>(py::ToInt(m.group(3)))});
    }
    if (outs.empty()) return std::nullopt;
    auto scalar = [](const std::string& ty) {
        return py::StartsWith(ty, "uint") ? std::string("uint") : std::string("float");
    };
    std::vector<std::string> fetch, apply;
    for (const Out& o : outs)
        fetch.push_back(
            absl::StrFormat("%s4 dm_fb%d [[color(%d), function_constant(dm_lfb%d)]]", scalar(o.ty), o.n, o.n, o.n));
    for (const Out& o : outs) {
        const std::string s = scalar(o.ty);
        const std::string w = o.ty.substr(s.size());
        const int width = w.empty() ? 1 : static_cast<int>(py::ToInt(w));
        const std::string zero = s == "uint" ? "0u" : "0.0f";
        std::string pad;
        for (int k = width; k < 4; ++k) pad += ", " + zero;
        const std::string sw = width < 4 ? "." + std::string("xyzw").substr(0, width) : "";
        apply.push_back(absl::StrFormat("if (dm_lfb%d) dm_r.%s = %s(%s(%s4(dm_r.%s%s), dm_fb%d, dm_lop, %du)%s);", o.n,
                                        o.nm, o.ty, s == "uint" ? "dm_logic" : "dm_logic_norm", s, o.nm, pad, o.n, o.n,
                                        sw));
    }
    std::vector<std::string> params = e.params;
    params.insert(params.end(), fetch.begin(), fetch.end());
    params.push_back(absl::StrCat("constant dm_logic_state& dm_lop [[buffer(", abi::kLogicBuffer, ")]]"));
    std::set<int> ns;
    for (const Out& o : outs) ns.insert(o.n);
    return std::pair{absl::StrCat(e.attr, "fragment ", e.ret, " ", e.name, "_l(", py::Join(params, ", "), ")\n{\n    ",
                                  e.ret, " dm_r = dm_body_", e.name, "(", e.args, ");\n    ", py::Join(apply, " "),
                                  "\n    return dm_r;\n}\n"),
                     ns};
}

}  // namespace logic
}  // namespace nxmac::dmsh
