// dmsh_entry.py: the entry points the build adds to a translated shader's MSL, or rewrites in it.
#include <functional>
#include <map>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "modules.h"
#include "re.h"

namespace nxmac::dmsh::entry {
namespace {

using py::Int;

// The closing brace of the body opened at b (depth counted from b), as the Python's loop over the characters: the
// last character when it never closes
Int CloseBrace(std::string_view src, Int b) {
    if (b < 0) throw py::Fatal("[RED] a function without a body");
    int depth = 0;
    Int e = b;
    for (; e < static_cast<Int>(src.size()); ++e) {
        depth += src[e] == '{';
        depth -= src[e] == '}';
        if (!depth) break;
    }
    if (e == static_cast<Int>(src.size())) --e;  // range() ran out: e is its last value
    return e;
}

struct StructMember {
    std::string ty, nm, at;
};

std::vector<StructMember> Members(std::string_view st) {
    static const re::Pattern p(R"(^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;)", re::M);
    std::vector<StructMember> out;
    for (const re::Match& m : p.finditer(st))
        out.push_back({std::string(m.group(1)), std::string(m.group(2)), std::string(m.group(3))});
    return out;
}

}  // namespace

bool VsWrites(const std::string& src, const Json& binds) {
    for (const Json& b : binds.array()) {
        const Json* rw = b.find("rw");
        if (rw && rw->truthy()) return true;
    }
    static const re::Pattern p(R"(access::(read_)?write\b)");
    return p.search(src).has_value();
}

std::string VsDiscard(const std::string& src) {
    static const re::Pattern p(R"(\bvertex\s+main0_out\s+main0\s*\((.*?)\)\s*\{)", re::S);
    auto m = p.search(src);
    if (!m) throw py::ValueError("vertex shader has no main0 entry for rasterizer discard");
    return absl::StrCat("\nvertex void main0_discard(", m->group(1), ")\n{",
                        py::Replace(src.substr(m->end()), "return out;", "return;"));
}

Json IntInputs(const std::string& src) {
    std::vector<std::pair<std::string, int>> ins;  // dict order: a name's first place, its last location
    for (const reflect::Attribute& a : reflect::Attributes(src)) {
        bool found = false;
        for (auto& [n, l] : ins) {
            if (n == a.name) {
                l = a.location;
                found = true;
            }
        }
        if (!found) ins.emplace_back(a.name, a.location);
    }
    Json out = Json::MakeObject();
    const Int i = py::Find(src, "vertex main0_out main0(");
    if (i < 0) return out;
    const std::string body = src.substr(i);
    auto ends_cast = [&](std::size_t at) {
        const std::string_view head = std::string_view(body).substr(0, at);
        return py::EndsWith(head, "as_type<int>(") || py::EndsWith(head, "as_type<uint>(");
    };
    for (const auto& [name, loc] : ins) {
        const re::Pattern use(absl::StrFormat(R"(in\.%s\b)", re::escape(name)));
        const re::Pattern copy(absl::StrFormat(R"(\s*float\s+(_\w+)\s*=\s*in\.%s\.[xyzw];$)", re::escape(name)));
        int nint = 0;
        bool bad = false;
        for (const re::Match& u : use.finditer(body)) {
            const Int ls = py::RFind(body, "\n", 0, static_cast<Int>(u.start())) + 1;
            const Int le = py::Find(body, "\n", static_cast<Int>(u.end()));
            auto m = copy.match(py::Slice(body, ls, le));
            if (!m) {
                if (ends_cast(u.start())) {
                    ++nint;
                    continue;
                }
                bad = true;
                break;
            }
            const std::string v(m->group(1));
            const re::Pattern word(absl::StrFormat(R"(\b%s\b)", re::escape(v)));
            const re::Pattern plain(absl::StrFormat(R"(\s*[\w.\[\]]+\s*=\s*%s;$)", re::escape(v)));
            const std::vector<re::Match> os = word.finditer(body);
            for (std::size_t k = 1; k < os.size(); ++k) {
                const re::Match& o = os[k];
                if (ends_cast(o.start())) {
                    ++nint;
                    continue;
                }
                const Int l2s = py::RFind(body, "\n", 0, static_cast<Int>(o.start())) + 1;
                const Int l2e = py::Find(body, "\n", static_cast<Int>(o.end()));
                if (plain.match(py::Slice(body, l2s, l2e))) continue;
                bad = true;
                break;
            }
            if (bad) break;
        }
        if (!bad && nint) out.set(absl::StrCat(loc), "uint");
    }
    return out;
}

std::string VsInit(const std::string& src) {
    const Int i = py::Find(src, "struct main0_out\n{\n");
    const Int j = py::Find(src, "\n};", i);
    Int k = py::Find(src, "main0_out out = {};");
    if (i < 0 || j < 0 || k < 0) return src;
    static const re::Pattern p(R"(\n    float4 (\w+) \[\[user\(locn\d+\)\]\];)");
    k += static_cast<Int>(std::string_view("main0_out out = {};").size());
    std::string add;
    for (const std::string& n : p.findall(py::Slice(src, i, j)))
        add += absl::StrCat("\n    out.", n, " = float4(0.0, 0.0, 0.0, 1.0);");
    return absl::StrCat(py::To(src, k), add, py::From(src, k));
}

std::string VsBaseInstance(const std::string& src_in) {
    constexpr std::string_view kWord = "(*spvDescriptorSet0.vp_c0)._m0[100].y";
    if (!py::Contains(src_in, kWord)) return src_in;
    const std::string src = py::Replace(src_in, kWord, "as_type<float>(gl_BaseInstance)");
    static const re::Pattern p(R"(\bvertex\s+\w+\s+main0\s*\((?:[^()]|\([^()]*\))*\))");
    return p.sub(
        [](const re::Match& m) {
            const std::string_view g = m.group(0);
            if (py::Contains(g, "[[base_instance]]")) return std::string(g);
            return absl::StrCat(g.substr(0, g.size() - 1), ", uint gl_BaseInstance [[base_instance]])");
        },
        src);
}

std::string DropPointSize(const std::string& src_in) {
    static const re::Pattern write(R"(\n    out\.gl_PointSize = [^;\n]*;)");
    static const re::Pattern vertex(R"(\bvertex\s+\w+\s+\w+\s*\()");
    std::string src = write.sub("", src_in);
    while (py::Contains(src, "out.gl_PointSize")) {
        const Int use = py::Find(src, "out.gl_PointSize");
        std::optional<re::Match> a;
        for (re::Match& m : vertex.finditer(src))
            if (static_cast<Int>(m.start()) < use) a = m;
        if (!a) throw py::Fatal("[RED] a point size output used outside a vertex function");
        const Int b = py::Find(src, "{", static_cast<Int>(a->end()));
        const Int e = CloseBrace(src, b);
        src = absl::StrCat(py::To(src, b + 1), "\n    float dmsh_point_size = 1.0;",
                           py::Replace(py::Slice(src, b + 1, e), "out.gl_PointSize", "dmsh_point_size"),
                           py::From(src, e));
    }
    return src;
}

bool LayerOut(std::string_view src) {
    const Int i = py::Find(src, "struct main0_out\n{\n");
    const Int j = py::Find(src, "\n};", i);
    return i >= 0 && j >= 0 && py::Contains(py::Slice(src, i, j), "[[render_target_array_index]]");
}

std::string VsLayer(const std::string& src) {
    if (!LayerOut(src)) return src;
    const Int i = py::Find(src, "struct main0_out\n{\n");
    const Int j = py::Find(src, "\n};", i);
    if (!py::Contains(py::Slice(src, i, j), "[[point_size]]")) return src;
    static const re::Pattern p(R"(\n    float gl_PointSize \[\[point_size\]\];)");
    return DropPointSize(absl::StrCat(py::To(src, i), p.sub("", py::Slice(src, i, j), 1), py::From(src, j)));
}

std::pair<std::string, Json> VsLayerEntry(const std::string& full) {
    const Int i = py::Find(full, "struct main0_out\n{\n");
    const Int j = py::Find(full, "\n};", i);
    if (i < 0 || j < 0 || !py::Contains(full, "gl_InstanceIndex") ||
        py::Contains(py::Slice(full, i, j), "[[render_target_array_index]]"))
        return {full, Json()};
    static const re::Pattern locs_p(R"(\n    float4 (\w+) \[\[user\(locn(\d+)\)\]\];)");
    std::map<std::string, Int> locs;
    for (const re::Match& m : locs_p.finditer(py::Slice(full, i, j)))
        locs[std::string(m.group(1))] = py::ToInt(m.group(2));
    static const re::Pattern write(R"(\n    out\.(\w+)\.([xyzw]) = as_type<float>\()");
    std::string name, comp;
    for (const re::Match& m : write.finditer(full)) {
        if (locs.count(std::string(m.group(1)))) {
            name = m.group(1);
            comp = m.group(2);
            break;
        }
    }
    if (name.empty()) return {full, Json()};
    static const re::Pattern psize(R"(\n    float gl_PointSize \[\[point_size\]\];)");
    std::string st = py::Replace(psize.sub("", py::Slice(full, i, j + 3)), "struct main0_out", "struct main0l_out", 1);
    st = absl::StrCat(py::To(st, -3), "\n    uint gl_Layer [[render_target_array_index]];\n};");
    std::string add = absl::StrCat("\n", st, "\n");
    for (const auto& [ep, nw] : {std::pair{"main0", "main0_l"}, std::pair{"main0_z", "main0_lz"}}) {
        auto a = re::search(absl::StrFormat(R"(\bvertex\s+main0_out\s+%s\s*\()", ep), full);
        if (!a) return {full, Json()};
        const Int b = py::Find(full, "{", static_cast<Int>(a->end()));
        const Int e = CloseBrace(full, b);
        std::string fn = py::Replace(py::Slice(full, static_cast<Int>(a->start()), e + 1), "main0_out", "main0l_out");
        fn = py::Replace(fn, absl::StrCat(" ", ep, "("), absl::StrCat(" ", nw, "("), 1);
        fn = DropPointSize(fn);
        add += absl::StrCat(
            "\n",
            py::Replace(fn, "return out;",
                        absl::StrCat("out.gl_Layer = as_type<uint>(out.", name, ".", comp, ");\n    return out;")),
            "\n");
    }
    Json info = Json::MakeObject();
    info.set("loc", locs[name]);
    info.set("comp", comp);
    return {full + add, info};
}

std::pair<std::string, std::string> VsDepth(const std::string& src) {
    static const re::Pattern p(R"(\bvertex\s+main0_out\s+main0\s*\()");
    auto m = p.search(src);
    if (!m) throw py::Fatal("[RED] vertex shader without a main0 main function, cannot add the depth entry points");
    const std::string body = src.substr(m->start());
    const std::string gl =
        py::Replace(body, "return out;",
                    "out.gl_Position.z = fma(out.gl_Position.z, 0.5, out.gl_Position.w * 0.5);\n    return out;");
    const std::string single = src.substr(0, m->start()) + gl;
    static const re::Pattern main0(R"(\bmain0\s*\()");
    return {single, absl::StrCat(single, "\n", main0.sub("main0_z(", body, 1))};
}

std::set<std::string> IntOutputs(const std::string& src, const std::string& body,
                                 const std::vector<std::string>& cols) {
    (void)src;
    static const re::Pattern types_p(R"(^\s*(u?int\d?|float\d?|bool)\s+(_\d+)\b)", re::M);
    static const re::Pattern defs_p(R"(^\s*(?:(?:u?int\d?|float\d?|bool)\s+)?(_\d+)\s*=\s*([^;]+);)", re::M);
    std::map<std::string, std::string> types;
    for (const re::Match& m : types_p.finditer(body)) types[std::string(m.group(2))] = m.group(1);
    std::map<std::string, std::vector<std::string>> defs;
    for (const re::Match& m : defs_p.finditer(body)) defs[std::string(m.group(1))].emplace_back(py::Strip(m.group(2)));
    static const re::Pattern tmp(R"(_\d+)");
    static const re::Pattern lit(R"(-?\d+u?)");
    static const re::Pattern choice(R"([^?]+\?\s*([^:]+?)\s*:\s*(.+))");
    static const re::Pattern as_int(R"(as_type<u?int\d?>\((.*)\))");
    static const re::Pattern uniform(R"(fp_c\d|\bin\.|^-?[0-9.])");
    using Kinds = std::set<std::string>;
    std::function<Kinds(const std::string&, std::set<std::string>&)> kinds = [&](const std::string& e,
                                                                                 std::set<std::string>& seen) -> Kinds {
        if (tmp.fullmatch(e)) {
            auto t = types.find(e);
            if (t != types.end() && py::StartsWith(t->second, "float")) return {"float"};
            if (seen.count(e)) return {};
            seen.insert(e);
            Kinds out;
            for (const std::string& x : defs[e]) {
                const Kinds k = kinds(x, seen);
                out.insert(k.begin(), k.end());
            }
            return out;
        }
        if (lit.fullmatch(e)) {
            const std::string_view digits = py::RStripChars(e, "u");
            const std::string_view mag = digits.substr(digits[0] == '-' ? 1 : 0);
            if (mag.size() > 9) return {};  // past 0x800000 whatever it is
            const Int v = py::ToInt(mag);
            return 0 < v && v < 0x800000 ? Kinds{"int"} : Kinds{};
        }
        if (auto t = choice.fullmatch(e)) {
            Kinds a = kinds(std::string(t->group(1)), seen);
            const Kinds b = kinds(std::string(t->group(2)), seen);
            a.insert(b.begin(), b.end());
            return a;
        }
        if (auto a = as_int.fullmatch(e)) return uniform.search(a->group(1)) ? Kinds{} : Kinds{"float"};
        return py::StartsWith(e, "(*spvDescriptorSet0") || py::StartsWith(e, "spvDescriptorSet0") ? Kinds{}
                                                                                                  : Kinds{"int"};
    };
    static const re::Pattern as_float(R"(as_type<float\d?>\((.*)\))");
    static const re::Pattern number(R"(-?[0-9.]+(?:e[-+]?\d+)?f?)");
    std::vector<std::pair<std::string, Kinds>> found;
    for (const std::string& nm : cols) {
        Kinds k;
        const re::Pattern w_p(absl::StrFormat(R"(\bout\.%s(?:\.\w+)? = ([^;]+);)", nm));
        for (const std::string& w0 : w_p.findall(body)) {
            const std::string w(py::Strip(w0));
            if (auto a = as_float.fullmatch(w)) {
                std::set<std::string> seen;
                const Kinds x = kinds(std::string(py::Strip(a->group(1))), seen);
                k.insert(x.begin(), x.end());
            } else if (number.fullmatch(w)) {
                const double f = std::abs(py::ToFloat(py::RStripChars(w, "f")));
                if (0 < f && f < 1.17549e-38) k.insert("int");
            } else {
                k.insert("float");
            }
        }
        found.emplace_back(nm, k);
    }
    std::set<std::string> only, any;
    for (const auto& [nm, k] : found) {
        if (k == Kinds{"int"}) only.insert(nm);
        if (k.count("int")) any.insert(nm);
    }
    return only.empty() ? any : only;
}

std::pair<std::optional<std::string>, Int> FsUint(const std::string& src) {
    static const re::Pattern main_p(R"(^.*\bfragment\s+main0_out\s+main0\s*\()", re::M);
    static const re::Pattern st_p(R"(struct main0_out\s*\{(.*?)\};)", re::S);
    auto m = main_p.search(src);
    auto st = st_p.search(src);
    if (!m || !st) return {std::nullopt, 0};
    const std::vector<StructMember> mem = Members(st->group(1));
    std::map<std::string, std::string> cols;
    std::vector<std::string> col_order;
    for (const StructMember& x : mem) {
        if (py::Contains(x.at, "color(") && py::StartsWith(x.ty, "float")) {
            if (!cols.count(x.nm)) col_order.push_back(x.nm);
            cols[x.nm] = x.ty;
        }
    }
    static const re::Pattern store_p(R"(out\.(\w+)(?:\.\w+)? = ([^;]+);)");
    static const re::Pattern as_float(R"(as_type<float\d?>\()");
    std::set<std::string> from_int;
    std::vector<std::string> writes;
    for (const re::Match& s : store_p.finditer(src)) {
        if (!cols.count(std::string(s.group(1))) || reflect::InWord(src, static_cast<Int>(s.start()))) continue;
        if (as_float.match(s.group(2))) from_int.insert(std::string(s.group(1)));
        writes.emplace_back(s.group(2));
    }
    std::set<std::string> flat;
    if (py::Contains(src, ", flat]]")) {
        static const re::Pattern flat_p(R"(\b(\w+) \[\[user\(locn\d+\), flat\]\])");
        for (const std::string& f : flat_p.findall(src)) flat.insert(f);
    }
    static const re::Pattern word(R"(\w+)");
    static const re::Pattern in_p(R"(in\.(\w+)(?:\.\w+)?)");
    auto passed = [&](const std::string& w0) {
        const std::string w(py::Strip(w0));
        std::optional<re::Match> d;
        if (word.fullmatch(w)) d = re::search(absl::StrFormat(R"(\bfloat\d? %s = (in\.\w+(?:\.\w+)?);)", w), src);
        auto i = in_p.fullmatch(d ? d->group(1) : std::string_view(w));
        return i && flat.count(std::string(i->group(1)));
    };
    std::string body = src.substr(m->start());
    const Int r = py::Find(body, "return out;");
    body = std::string(py::To(body, r + 1 ? r + 1 : static_cast<Int>(body.size())));
    const std::set<std::string> ints = IntOutputs(src, body, col_order);
    static const re::Pattern constant(R"(-?[0-9.]+f?|float\d\([-0-9., ]*\))");
    bool all_const = true, all_passed = true;
    for (const std::string& w : writes) {
        all_const = all_const && constant.fullmatch(py::Strip(w)).has_value();
        all_passed = all_passed && passed(w);
    }
    if (from_int.empty() && ints.empty() && !(!writes.empty() && (all_const || all_passed))) return {std::nullopt, 0};
    auto uty = [](const std::string& ty) { return "u" + py::Replace(ty, "float", "int"); };
    std::optional<side::Entry> e;
    try {
        e = side::FindEntry(src, "main0");
    } catch (const py::ValueError&) {
        e.reset();
    }
    if (!e) return {std::nullopt, 0};
    auto entry = [&](const std::string& sfx, const std::set<std::string>& as_uint) {
        std::string st_text = absl::StrCat("struct main0_out_", sfx, "\n{\n");
        std::vector<std::string> conv;
        for (const StructMember& x : mem) {
            st_text += absl::StrCat("    ", as_uint.count(x.nm) ? uty(cols[x.nm]) : x.ty, " ", x.nm, " ", x.at, ";\n");
            conv.push_back(as_uint.count(x.nm) ? absl::StrCat("as_type<", uty(cols[x.nm]), ">(out.", x.nm, ")")
                                               : absl::StrCat("out.", x.nm));
        }
        st_text += "};\n\n";
        return absl::StrCat("\n", st_text,
                            side::Calling(*e, "main0_" + sfx, "main0_out_" + sfx,
                                          absl::StrCat("return main0_out_", sfx, "{", py::Join(conv, ", "), "};")));
    };
    std::set<std::string> all_cols(col_order.begin(), col_order.end());
    std::string out = entry("u", all_cols);
    Int mask = 0;
    const std::set<std::string>& as_int = !ints.empty() ? ints : from_int;
    bool some_float = false;
    for (const std::string& c : all_cols) some_float = some_float || !as_int.count(c);
    if (!as_int.empty() && some_float) {
        out += entry("m", as_int);
        static const re::Pattern color(R"(color\((\d+)\))");
        for (const StructMember& x : mem) {
            if (as_int.count(x.nm)) {
                auto c = color.search(x.at);
                if (!c) throw py::Fatal("[RED] an integer output without a colour");
                mask += Int(1) << py::ToInt(c->group(1));
            }
        }
    }
    return {out, mask};
}

std::optional<std::string> FsDual(const std::string& src) {
    static const re::Pattern st_p(R"(struct main0_out\s*\{(.*?)\};)", re::S);
    auto st = st_p.search(src);
    if (!st) return std::nullopt;
    std::optional<side::Entry> e;
    try {
        e = side::FindEntry(src, "main0");
    } catch (const py::ValueError&) {
        e.reset();
    }
    if (!e) return std::nullopt;
    const std::vector<StructMember> mem = Members(st->group(1));
    std::vector<std::string> ats;
    for (const StructMember& x : mem)
        if (py::Contains(x.at, "color(")) ats.push_back(x.at);
    std::sort(ats.begin(), ats.end());
    if (ats != std::vector<std::string>{"[[color(0)]]", "[[color(1)]]"}) return std::nullopt;
    std::string st_text = "struct main0_out_d\n{\n";
    std::vector<std::string> conv;
    for (const StructMember& x : mem) {
        st_text +=
            absl::StrCat("    ", x.ty, " ", x.nm, " ", x.at == "[[color(1)]]" ? "[[color(0), index(1)]]" : x.at, ";\n");
        conv.push_back("out." + x.nm);
    }
    st_text += "};\n\n";
    return absl::StrCat(
        "\n", st_text,
        side::Calling(*e, "main0_d", "main0_out_d", absl::StrCat("return main0_out_d{", py::Join(conv, ", "), "};")));
}

}  // namespace nxmac::dmsh::entry
