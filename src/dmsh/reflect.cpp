// dmsh_reflect.py: a translated shader's vertex attributes, bindings, driver segment reads and raster facts, and the
// rewrite of its pool arrays.
#include <algorithm>
#include <map>

#include "abi.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "modules.h"
#include "re.h"

namespace nxmac::dmsh::reflect {
namespace {

using py::Int;

constexpr Int kPoolMin = 33;

const std::string_view kPoolMark =
    R"(template<typename P> static inline __attribute__((always_inline)) uint dm_pool_mark(constant P* p, uint i)
{
    device uint* w = reinterpret_cast<device uint*>(p->seen) + (i >> 5);
    uint b = 1u << (i & 31u);
    if (!(*w & b)) atomic_fetch_or_explicit(reinterpret_cast<device atomic_uint*>(w), b, memory_order_relaxed);
    return i;
}

)";

std::optional<Int> Ssbo(std::string_view stage) {
    if (stage == "vs") return abi::kC0SsboVs;
    if (stage == "cs") return abi::kC0SsboCs;
    if (stage == "gs") return abi::kC0SsboGs;
    if (stage == "ps") return abi::kC0SsboPs;
    if (stage == "ts") return abi::kC0SsboTs;
    if (stage == "ms") return abi::kC0SsboMs;
    return std::nullopt;
}

int CompIndex(std::string_view c) { return static_cast<int>(std::string_view("xyzw").find(c)); }

// A handle's hex digits as Python's int(..., 16); handles are 8 digits, so a longer one is not a name the translator
// writes and decodes as nothing
std::optional<Int> Hex(std::string_view s) {
    if (s.size() > 15) return std::nullopt;
    return py::ToInt(s, 16);
}

Json Member(const std::string& msl_name, const std::string& ty, Int aid) {
    Json e = Json::MakeObject();
    e.set("msl_name", msl_name);
    e.set("msl_type", ty);
    e.set("arg_id", aid);
    return e;
}

}  // namespace

std::vector<Attribute> Attributes(std::string_view src) {
    const Int i = py::Find(src, "[[attribute("), j = py::RFind(src, "[[attribute(");
    if (i < 0) return {};
    const Int end = py::Find(src, "\n", j);
    static const re::Pattern p(R"((\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\])");
    std::vector<Attribute> out;
    for (const re::Match& m :
         p.finditer(py::Slice(src, py::RFind(src, "\n", 0, i) + 1, end >= 0 ? end : static_cast<Int>(src.size()))))
        out.push_back({std::string(m.group(1)), std::string(m.group(2)), static_cast<int>(py::ToInt(m.group(3)))});
    return out;
}

bool InWord(std::string_view src, Int i) { return i > 0 && (src[i - 1] == '_' || py::IsAlnum(src[i - 1])); }

bool HasWord(std::string_view src, std::string_view text) {
    Int i = py::Find(src, text);
    while (i >= 0) {
        if (!InWord(src, i)) return true;
        i = py::Find(src, text, i + 1);
    }
    return false;
}

Json RasterFacts(std::string_view stage, const std::string& src) {
    static const re::Pattern out_p(R"(struct main0_out\s*\{(.*?)\};)", re::S);
    if (stage == "ps") {
        auto out = out_p.search(src);
        const std::string_view body = out ? out->group(1) : std::string_view();
        static const re::Pattern color(R"(\[\[color\((\d+)\)\]\])");
        std::set<Int> colors;
        for (const std::string& c : color.findall(body)) colors.insert(py::ToInt(c));
        Json r = Json::MakeObject();
        r.set("discard", static_cast<int>(py::Contains(src, "discard_fragment")));
        r.set("sample_mask", static_cast<int>(py::Contains(body, "sample_mask")));
        r.set("primitive_id", static_cast<int>(py::Contains(src, "[[primitive_id]]")));
        Json list = Json::MakeArray();
        for (Int c : colors) list.push_back(c);
        r.set("colors", list);
        return r;
    }
    if (stage != "vs") return Json::MakeObject();
    Json screen = Json::MakeArray();
    for (int k : fetch::ScreenVaryings(src)) screen.push_back(k);
    auto fail = [&] {
        Json r = Json::MakeObject();
        r.set("pos_attr", -1);
        r.set("screen", screen);
        return r;
    };
    static const re::Pattern body_p(R"(\bvertex\s+\w+\s+\w+\s*\(.*?\)\s*\{(.*)\}\s*$)", re::S);
    static const re::Pattern branchy(R"(\b(if|for|while|switch|do)\b|clip_distance|ClipDistance)");
    auto body = body_p.search(src);
    if (!body || branchy.search(body->group(1))) return fail();
    const std::string b(body->group(1));
    std::map<std::string, int> attr;
    for (const Attribute& a : Attributes(src)) attr[a.name] = a.location;
    static const re::Pattern var_p(R"(float\s+(_\w+)\s*=\s*in\.(\w+)\.([xyzw]);)");
    std::map<std::string, std::pair<std::string, std::string>> var;
    for (const re::Match& m : var_p.finditer(b))
        var[std::string(m.group(1))] = {std::string(m.group(2)), std::string(m.group(3))};
    static const re::Pattern pos_p(R"(out\.gl_Position\.([xyzw])\s*=\s*([^;]+);)");
    std::map<std::string, std::string> last;
    for (const re::Match& m : pos_p.finditer(b)) last[std::string(m.group(1))] = std::string(py::Strip(m.group(2)));
    auto src_of = [&](const std::string& c) -> std::optional<std::pair<std::string, std::string>> {
        auto l = last.find(c);
        if (l == last.end()) return std::nullopt;
        auto v = var.find(l->second);
        if (v == var.end()) return std::nullopt;
        return v->second;
    };
    const auto m0 = src_of("x");
    if (!m0 || !attr.count(m0->first)) return fail();
    for (const char* c : {"x", "y", "z"}) {
        if (src_of(c) != std::pair{m0->first, std::string(c)}) return fail();
    }
    std::string w;
    if (last.count("w") && last["w"] == "1.0") {
        w = "one";
    } else if (src_of("w") == std::pair{m0->first, std::string("w")}) {
        w = "attr";
    } else {
        return fail();
    }
    auto ost = out_p.search(src);
    static const re::Pattern locn_p(R"((m_\d+) \[\[user\(locn(\d+)\)\]\])");
    std::vector<std::pair<std::string, Int>> locn;  // dict order
    std::map<std::string, std::size_t> locn_at;
    for (const re::Match& m : locn_p.finditer(ost ? ost->group(1) : std::string_view())) {
        const std::string mem(m.group(1));
        if (auto it = locn_at.find(mem); it != locn_at.end()) {
            locn[it->second].second = py::ToInt(m.group(2));
        } else {
            locn_at[mem] = locn.size();
            locn.emplace_back(mem, py::ToInt(m.group(2)));
        }
    }
    static const re::Pattern olast_p(R"(out\.(m_\d+)\.([xyzw])\s*=\s*([^;]+);)");
    std::map<std::pair<std::string, std::string>, std::string> olast;
    for (const re::Match& m : olast_p.finditer(b))
        olast[{std::string(m.group(1)), std::string(m.group(2))}] = std::string(py::Strip(m.group(3)));
    auto get = [&](const std::string& mem, const char* c) {
        auto it = olast.find({mem, c});
        return it == olast.end() ? std::string() : it->second;
    };
    struct Fma {
        std::pair<std::string, std::string> v;
        Json a, c;
    };
    auto fm = [&](const std::string& v) -> std::optional<Fma> {
        const re::Pattern p(absl::StrFormat(
            R"(float %s = fma\((_\w+), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw]), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw])\);)",
            re::escape(v)));
        auto m = p.search(b);
        if (!m || !var.count(std::string(m->group(1)))) return std::nullopt;
        Json a = Json::MakeArray(), c = Json::MakeArray();
        a.push_back(py::ToInt(m->group(2)) - 3);
        a.push_back(py::ToInt(m->group(3)));
        a.push_back(CompIndex(m->group(4)));
        c.push_back(py::ToInt(m->group(5)) - 3);
        c.push_back(py::ToInt(m->group(6)));
        c.push_back(CompIndex(m->group(7)));
        return Fma{var[std::string(m->group(1))], a, c};
    };
    Json uv = Json::MakeObject(), aff = Json::MakeObject();
    for (const auto& [mem, k] : locn) {
        auto vx = var.find(get(mem, "x"));
        auto vy = var.find(get(mem, "y"));
        if (vx != var.end() && vy != var.end() && vx->second.first == vy->second.first &&
            attr.count(vx->second.first) && vx->second.second == "x" && vy->second.second == "y") {
            uv.set(absl::StrCat(k), attr[vx->second.first]);
            continue;
        }
        auto fx = fm(get(mem, "x")), fy = fm(get(mem, "y"));
        if (fx && fy && fx->v.first == fy->v.first && attr.count(fx->v.first) && fx->v.second == "x" &&
            fy->v.second == "y" &&
            std::min({fx->a.array()[0].as_int(), fx->c.array()[0].as_int(), fy->a.array()[0].as_int(),
                      fy->c.array()[0].as_int()}) >= 0) {
            Json e = Json::MakeObject();
            e.set("attr", attr[fx->v.first]);
            e.set("sx", fx->a);
            e.set("ox", fx->c);
            e.set("sy", fy->a);
            e.set("oy", fy->c);
            aff.set(absl::StrCat(k), e);
        }
    }
    Json r = Json::MakeObject();
    r.set("pos_attr", attr[m0->first]);
    r.set("pos_w", w);
    r.set("uv_from_attr", uv);
    r.set("uv_affine", aff);
    r.set("screen", screen);
    return r;
}

std::optional<Tex> DecodeTex(std::string_view name) {
    static const re::Pattern p(R"([a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+))");
    static const re::Pattern image(R"([a-z]+_i)");
    auto m = p.match(name);
    if (!m) return std::nullopt;
    const Int slots = m->has(1) && !m->group(1).empty() ? py::ToInt(m->group(1)) : -1;
    const std::optional<Int> hh = Hex(m->group(2));
    if (!hh) return std::nullopt;
    const Int h = *hh;
    const Int tb = slots >= 0 ? (slots & 0xffff) : 2;
    const Int sb = slots >= 0 && (slots >> 16) ? (slots >> 16) - 1 : tb;
    const Int o0 = h & 0x3fff, o1 = (h >> 14) & 0x3fff, ty = h >> 28;
    auto slot = [](Int word, Int base, Int cb) -> std::optional<Int> {
        if (cb == 2 && word * 4 >= base && (word * 4 - base) % abi::kC2Row == 0) return (word * 4 - base) / abi::kC2Row;
        return std::nullopt;
    };
    std::vector<Int> words{o0};
    if (ty) words.push_back(o1);
    Tex out;
    out.type = ty;
    if (image.match(name)) {
        for (Int x : words) {
            if (abi::kC2Img <= x * 4 && x * 4 < abi::kC2Img + abi::kC2Row * abi::kC2ImgSlots &&
                slot(x, abi::kC2Img, tb)) {
                out.t = slot(x, abi::kC2Img, tb);
                break;
            }
        }
        return out;
    }
    const std::vector<Int> tw = ty == 3 ? std::vector<Int>{o0} : words;
    for (Int x : tw) {
        if (slot(x, abi::kC2Tex, tb) && x * 4 < abi::kC2Septex) {
            out.t = slot(x, abi::kC2Tex, tb);
            break;
        }
    }
    if (!out.t) {
        for (Int x : tw) {
            const std::optional<Int> sl = slot(x, abi::kC2Septex, tb);
            if (sl && x * 4 < abi::kC2Samp) {
                out.t = abi::kSeptexSlot0 + *sl;
                break;
            }
        }
    }
    if (ty != 3) {
        for (Int x : words) {
            if (x * 4 >= abi::kC2Samp && slot(x, abi::kC2Samp, sb)) {
                out.s = slot(x, abi::kC2Samp, sb);
                break;
            }
        }
    }
    return out;
}

std::optional<Int> ConstSampler(std::string_view name) {
    static const re::Pattern p(R"([a-z]+_t\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+))");
    auto m = p.match(name);
    if (!m) return std::nullopt;
    const std::optional<Int> h = Hex(m->group(2));
    if (!h || (*h >> 28) != 3) return std::nullopt;
    return (*h >> 14) & 0x3fff;
}

std::optional<std::pair<UboPart, UboPart>> DecodeUboTex(std::string_view name) {
    static const re::Pattern p(R"([a-z]+_t\w*?_cb(\d+)_([0-9A-F]+))");
    auto m = p.match(name);
    if (!m) return std::nullopt;
    const Int slots = py::ToInt(m->group(1));
    const std::optional<Int> hh = Hex(m->group(2));
    if (!hh) return std::nullopt;
    const Int h = *hh;
    const Int tb = slots & 0xffff;
    const Int sb = (slots >> 16) ? (slots >> 16) - 1 : tb;
    const Int o0 = h & 0x3fff, o1 = (h >> 14) & 0x3fff, ty = h >> 28;
    if (!(3 <= tb && tb <= 16) || !(ty == 0 || ty == 1 || ty == 3) || (ty == 1 && sb != 2 && !(3 <= sb && sb <= 16)))
        return std::nullopt;
    const UboPart t{"", tb - 3, o0 * 4};
    UboPart s;
    if (ty == 3)
        s = {"id", 0, o1};
    else if (ty && sb == 2)
        s = {"c2", 0, o1 * 4};
    else if (ty)
        s = {"", sb - 3, o1 * 4};
    else
        s = t;
    return std::pair{t, s};
}

std::optional<std::pair<Int, Int>> DecodeUboImg(std::string_view name) {
    static const re::Pattern p(R"([a-z]+_i\w*?_cb(\d+)_([0-9A-F]+))");
    auto m = p.match(name);
    if (!m) return std::nullopt;
    const Int cb = py::ToInt(m->group(1));
    if (!(3 <= cb && cb <= 16)) return std::nullopt;
    const std::optional<Int> h = Hex(m->group(2));
    if (!h) return std::nullopt;
    return std::pair{cb - 3, *h * 4};
}

std::optional<std::pair<Int, std::optional<Int>>> C2Array(std::string_view name, std::string_view ty) {
    static const re::Pattern p(R"([a-z]+_t\w*?_cb(\d+)_([0-9A-F]+))");
    auto m = p.fullmatch(py::EndsWith(name, "Smplr") ? name.substr(0, name.size() - 5) : name);
    if (!m || !py::StartsWith(ty, "array<")) return std::nullopt;
    const Int slots = py::ToInt(m->group(1));
    const std::optional<Int> hh = Hex(m->group(2));
    if (!hh) return std::nullopt;
    const Int h = *hh;
    if ((h >> 28) == 0 && slots == 2) return std::pair{(h & 0x3fff) * 4, std::optional<Int>()};
    if ((h >> 28) == 1 && (slots & 0xffff) == 2 && (slots >> 16) - 1 == 2)
        return std::pair{(h & 0x3fff) * 4, std::optional<Int>(((h >> 14) & 0x3fff) * 4)};
    return std::nullopt;
}

std::string PoolArrays(const std::string& src_in) {
    static const re::Pattern body_p(R"(struct spvDescriptorSetBuffer0\s*\{(.*?)\};)", re::S);
    auto body = body_p.search(src_in);
    if (!body) return src_in;
    struct Pool {
        std::string elem;
        Int n;
        bool marked;
    };
    std::vector<std::string> order;
    std::map<std::string, Pool> pools;
    static const re::Pattern arr(R"(^(\s*)array<(.+), (\d+)> (\w+) \[\[id\()", re::M);
    std::string members = arr.sub(
        [&](const re::Match& m) -> std::string {
            const std::string name(m.group(4));
            if (py::ToInt(m.group(3)) < kPoolMin ||
                !(py::EndsWith(name, "_cb0_40000000") || DecodeUboTex(py::RemoveSuffix(name, "Smplr"))))
                return std::string(m.group(0));
            if (!pools.count(name)) order.push_back(name);
            pools[name] = {std::string(m.group(2)), py::ToInt(m.group(3)),
                           m.group(2) != "sampler" && py::EndsWith(name, "_cb0_40000000")};
            return absl::StrCat(m.group(1), "constant spvPool_", name, "* ", name, " [[id(");
        },
        body->group(1));
    if (pools.empty()) return src_in;
    Int aid = 0;
    static const re::Pattern member(R"(^(\s*)(.+?)\s+(\w+)\s*\[\[id\((\d+)\)((?:\s*,\s*\w+\(\d+\))*)\]\];)", re::M);
    static const re::Pattern arr_n(R"(array<.+, (\d+)>)");
    members = member.sub(
        [&](const re::Match& m) {
            auto a = arr_n.fullmatch(py::Strip(m.group(2)));
            std::string out = absl::StrCat(m.group(1), m.group(2), " ", m.group(3), " [[id(", aid, m.group(5), ")]];");
            aid += a ? py::ToInt(a->group(1)) : 1;
            return out;
        },
        members);
    std::string structs;
    bool marked = false;
    for (const std::string& nm : order) {
        structs += absl::StrCat("struct spvPool_", nm, "\n{\n    uint n;\n    ulong seen;\n    array<", pools[nm].elem,
                                ", 1> e;\n};\n\n");
        marked = marked || pools[nm].marked;
    }
    if (marked) structs = std::string(kPoolMark) + structs;
    const std::size_t i = body->start();
    std::string src = absl::StrCat(src_in.substr(0, i), structs, src_in.substr(i, body->start(1) - i), members,
                                   src_in.substr(body->end(1)));
    for (const std::string& nm : order) {
        const re::Pattern use(absl::StrFormat(R"(([\w.]+)\.%s\[)", re::escape(nm)));
        std::string out;
        std::size_t pos = 0;
        for (const re::Match& m : use.finditer(src)) {
            if (m.start() < pos) continue;
            int depth = 1;
            std::size_t j = m.end();
            while (depth) {
                if (j >= src.size()) throw py::ValueError(absl::StrCat(nm, ": unbalanced index"));
                depth += src[j] == '[' ? 1 : src[j] == ']' ? -1 : 0;
                ++j;
            }
            const std::string base = absl::StrCat(m.group(1), ".", nm);
            const std::string index =
                absl::StrCat("min(uint(", src.substr(m.end(), j - 1 - m.end()), "), ", base, "->n)");
            out += src.substr(pos, m.start() - pos);
            out += absl::StrCat("(&", base, "->e[0])[",
                                pools[nm].marked ? absl::StrCat("dm_pool_mark(", base, ", ", index, ")") : index, "]");
            pos = j;
        }
        out += src.substr(pos);
        src = std::move(out);
        const Int names = pools[nm].marked ? 3 : 2;
        const re::Pattern word(absl::StrFormat(R"(\b%s\b)", re::escape(nm)));
        const re::Pattern read(absl::StrFormat(R"(\.%s->e\[0\]\)\[)", re::escape(nm)));
        const Int left =
            static_cast<Int>(word.finditer(src).size()) - names * static_cast<Int>(read.finditer(src).size()) - 1;
        if (left)
            throw py::ValueError(absl::StrFormat("%s: %d uses of the pool array the rewrite does not cover", nm, left));
    }
    const std::string own = py::Replace(src, kPoolMark, "");
    static const re::Pattern stores(
        R"(\bdiscard_fragment\b|\[\[\s*depth\(|\[\[\s*sample_mask\s*\]\]|\[\[\s*stencil\s*\]\]|\.write\(|\batomic_\w+_explicit\b|^\s*device\s)",
        re::M);
    static const re::Pattern frag(R"(^(?<!\]\] )fragment\s)", re::M);
    if (marked && !stores.search(own)) src = frag.sub("[[early_fragment_tests]] fragment ", src);
    return src;
}

Json Reflect(std::string_view key, std::string_view stage, const std::string& msl, const Json& res) {
    static const re::Pattern body_p(R"(struct spvDescriptorSetBuffer0\s*\{(.*?)\};)", re::S);
    static const re::Pattern member_p(R"(^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)(?:\s*,\s*\w+\(\d+\))*\]\];)", re::M);
    static const re::Pattern cbank(R"([a-z]+_c(\d+))");
    static const re::Pattern sbank(R"([a-z]+_s(\d+))");
    static const re::Pattern image(R"([a-z]+_i)");
    auto body = body_p.search(msl);
    const std::vector<re::Match> members = member_p.finditer(body ? body->group(1) : std::string_view());
    if (body && static_cast<Int>(members.size()) != py::Count(body->group(1), "[[id("))
        throw py::ValueError(absl::StrCat(key, ": an argument buffer member the reflection cannot read"));
    Json binds = Json::MakeArray();
    int border_bits = 0;
    for (const re::Match& m : members) {
        const std::string ty(py::Strip(m.group(1)));
        std::string nm(m.group(2));
        const Int aid = py::ToInt(m.group(3));
        Json e = Member(nm, ty, aid);
        std::optional<re::Match> pool;
        if (ty == absl::StrCat("constant spvPool_", nm, "*")) {
            pool = re::search(absl::StrFormat(R"(struct spvPool_%s\s*\{\s*uint n;\s*ulong seen;\s*array<(.+), \d+> e;)",
                                              re::escape(nm)),
                              msl);
        }
        const std::string smplr_twin = absl::StrCat("Smplr", abi::kBorderTwin);
        if (ty == "sampler" && py::EndsWith(nm, smplr_twin)) {
            nm = nm.substr(0, nm.size() - abi::kBorderTwin.size());
            e.set("border_bit", border_bits++);
        }
        const bool smplr = py::EndsWith(nm, "Smplr");
        const std::string unsuffixed = smplr ? nm.substr(0, nm.size() - 5) : nm;
        // what the branches below test, each a function of the name alone (the Python tests them in turn)
        const std::optional<re::Match> c = cbank.fullmatch(nm), sb = sbank.fullmatch(nm);
        const std::optional<std::pair<UboPart, UboPart>> u = DecodeUboTex(unsuffixed);
        const std::optional<std::pair<Int, Int>> ui = DecodeUboImg(nm);
        const std::optional<std::pair<Int, std::optional<Int>>> c2a = C2Array(nm, ty);
        if (nm == abi::kBorderMember) {
            e.set("kind", abi::kKindBorder);
        } else if (auto hlp = geometry::Helper(nm)) {
            e.set("kind", abi::kKindGeometry);
            e.set("reg", *hlp);
        } else if (pool && !py::EndsWith(nm, "_cb0_40000000")) {
            const auto ts = DecodeUboTex(py::RemoveSuffix(nm, "Smplr"));
            if (!ts) throw py::ValueError(absl::StrCat(key, ": a handle table whose name does not decode"));
            const auto& [ut, us] = *ts;
            e.set("kind", pool->group(1) == "sampler" ? abi::kKindSampHtab : abi::kKindTexHtab);
            e.set("elem", pool->group(1));
            e.set("reg", ut.a);
            e.set("off", ut.b);
            if (us.kind == "c2") {
                e.set("soff", us.b);
                e.set("sreg", abi::kSregC2);
            }
        } else if (pool) {
            e.set("kind", pool->group(1) == "sampler" ? abi::kKindSampPool : abi::kKindTexPool);
            e.set("elem", pool->group(1));
        } else if (nm == "support_buffer") {
            e.set("kind", abi::kKindSupport);
        } else if (c) {
            const Int n = py::ToInt(c->group(1));
            if (3 <= n && n <= 16) {
                Int fixed = 0;
                if (const Json* cbufs = res.find("cbufs")) {
                    for (const Json& b : cbufs->array()) {
                        if (b.at("slot").as_int() == n) {
                            fixed = b.at("fixed").as_int();
                            break;
                        }
                    }
                } else {
                    throw py::ValueError("res.json without cbufs");
                }
                e.set("kind", abi::kKindCbuf);
                e.set("reg", n - 3);
                e.set("fixed", fixed);
            } else if (n == abi::kCbsizeBank) {
                e.set("kind", abi::kKindCbsize);
            } else {
                e.set("kind", n == 1 ? abi::kKindC1 : abi::kKindDrvcb);
                e.set("bank", n);
            }
        } else if (sb) {
            e.set("kind", abi::kKindSbuf);
            e.set("index", py::ToInt(sb->group(1)));
        } else if (u) {
            const auto& [ut, us] = *u;
            const bool other = !(us.kind == "id" || us.kind == "c2" || us == ut);
            if (smplr && us.kind == "c2") {
                e.set("kind", abi::kKindSampUbo);
                e.set("reg", ut.a);
                e.set("off", ut.b);
                e.set("soff", us.b);
                e.set("sreg", abi::kSregC2);
            } else if (smplr) {
                if (us.kind == "id") {
                    e.set("kind", abi::kKindSampId);
                    e.set("reg", us.b);
                } else {
                    e.set("kind", abi::kKindSampUbo);
                    e.set("reg", us.a);
                    e.set("off", us.b);
                    if (other) {
                        e.set("soff", ut.b);
                        e.set("sreg", ut.a);
                    }
                }
            } else if (py::Contains(ty, "texture_buffer")) {
                e.set("kind", abi::kKindTbufUbo);
                e.set("reg", ut.a);
                e.set("off", ut.b);
            } else {
                e.set("kind", abi::kKindTexUbo);
                e.set("reg", ut.a);
                e.set("off", ut.b);
                if (other) {
                    e.set("soff", us.b);
                    e.set("sreg", us.a);
                }
            }
        } else if (ui) {
            e.set("kind", abi::kKindImgUbo);
            e.set("reg", ui->first);
            e.set("off", ui->second);
            if (HasWord(msl, nm + ".atomic_")) e.set("atomic", true);
        } else if (c2a) {
            e.set("kind", smplr ? abi::kKindSampC2a : abi::kKindTexC2a);
            e.set("off", c2a->first);
            if (c2a->second) e.set("soff", *c2a->second);
        } else if (smplr) {
            const std::optional<Tex> d = DecodeTex(unsuffixed);
            if (!d)
                throw std::runtime_error(absl::StrCat(key, ": cannot unpack non-iterable NoneType object (the sampler ",
                                                      nm, " does not decode)"));
            if (d->type == 3) {
                e.set("kind", abi::kKindSampId);
                const std::optional<Int> cs = ConstSampler(unsuffixed);
                e.set("reg", cs ? Json(*cs) : Json());
            } else if (d->type) {
                e.set("kind", abi::kKindSamp);
                e.set("reg", d->s ? Json(*d->s) : Json());
            } else {
                e.set("kind", abi::kKindSampTex);
                e.set("reg", d->t ? Json(*d->t) : Json());
            }
        } else {
            const std::optional<Tex> d = DecodeTex(nm);
            if (!d) {
                e.set("kind", abi::kKindUnknown);
            } else {
                e.set("kind", image.match(nm)                      ? abi::kKindImg
                              : py::Contains(ty, "texture_buffer") ? abi::kKindTbuf
                                                                   : abi::kKindTex);
                e.set("reg", d->t ? Json(*d->t) : Json());
            }
            const std::string kind = e.at("kind").as_string();
            if (kind == abi::kKindImg && HasWord(msl, nm + ".atomic_")) e.set("atomic", true);
            if (kind == abi::kKindTex) {
                const Json* reg = e.find("reg");
                bool in = false;
                if (const Json* f1 = res.find("fetch1d")) {
                    for (const Json& r : f1->array()) in = in || (reg && r == *reg);
                }
                if (in) {
                    e.set("fetch1d", true);
                    if (HasWord(msl, nm + ".get_width(")) e.set("fetch1d_size", true);
                }
            }
        }
        binds.push_back(e);
    }
    const std::optional<Int> base = Ssbo(stage);
    std::vector<const Json*> sb_sorted;
    for (const Json& b : res.at("sbufs").array()) sb_sorted.push_back(&b);
    std::stable_sort(sb_sorted.begin(), sb_sorted.end(), [](const Json* a, const Json* b) {
        return a->at("binding").as_int() < b->at("binding").as_int();
    });
    static const re::Pattern write_note(R"re(operation "(Store|Atomic\w*)" traced to no storage buffer)re");
    for (Json& e : binds.array()) {
        if (e.at("kind").as_string() != abi::kKindSbuf) continue;
        const Int index = e.at("index").as_int();
        const Json* b = index < static_cast<Int>(sb_sorted.size()) && index >= 0 ? sb_sorted[index] : nullptr;
        std::optional<Int> off;
        if (b && b->at("cb_slot").as_int() == 0) off = b->at("cb_offset").as_int() * 4;
        if (b && b->at("cb_slot").as_int() == abi::kGmemMapCbSlot) {
            if (b->at("cb_offset").as_int() == abi::kGmemLogCbOffset) {
                e.set("kind", abi::kKindGmemLog);
                e.set("rw", true);
                continue;
            }
            bool rw = false;
            if (const Json* notes = res.find("notes")) {
                for (const Json& n : notes->array()) rw = rw || write_note.search(n.as_string()).has_value();
            }
            e.set("kind", abi::kKindGmemMap);
            e.set("rw", rw);
            continue;
        }
        if (stage == "cs" && off && abi::kC0CsUbo <= *off && *off < abi::kC0CsUbo + abi::kC0Row * abi::kC0CsUboSlots &&
            (*off - abi::kC0CsUbo) % abi::kC0Row == 0) {
            e.set("kind", abi::kKindUboPtr);
            e.set("reg", (*off - abi::kC0CsUbo) / abi::kC0Row);
        } else if (b && b->at("cb_slot").as_int() >= 3) {
            e.set("kind", abi::kKindSbufUbo);
            e.set("reg", b->at("cb_slot").as_int() - 3);
            e.set("off", b->at("cb_offset").as_int() * 4);
        } else {
            if (off && base && 0 <= *off - *base && *off - *base < abi::kC0Row * abi::kC0SsboSlots &&
                (*off - *base) % abi::kC0Row == 0) {
                e.set("reg", (*off - *base) / abi::kC0Row);
            } else {
                e.set("reg", Json());
            }
        }
        e.set("rw", b != nullptr && (b->at("flags").as_int() & 1) != 0);
    }
    return binds;
}

Json DrvReads(const std::string& src) {
    Json out = Json::MakeObject();
    for (int bank : {0, 2}) {
        const re::Pattern p(absl::StrFormat(R"(fp_c%d\)\._m0\[([^\]]+)\])", bank));
        std::set<Int> offs;
        bool dyn = false;
        for (const re::Match& m : p.finditer(src)) {
            const std::string_view idx = py::RStripChars(m.group(1), "u");
            if (py::IsDigit(idx))
                offs.insert(py::ToInt(idx) * 16);
            else
                dyn = true;
        }
        if (!offs.empty() || dyn) {
            Json d = Json::MakeObject();
            Json list = Json::MakeArray();
            for (Int o : offs) list.push_back(o);
            d.set("vec4_offsets", list);
            d.set("dynamic", dyn);
            out.set(absl::StrCat(bank), d);
        }
    }
    return out;
}

Json Unfillable(const Json& dr, std::string_view stage) {
    std::vector<std::pair<Int, Int>> fill0, fill2;
    {
        std::vector<std::pair<std::string, Int>> ssbo = {{"vs", abi::kC0SsboVs}, {"cs", abi::kC0SsboCs},
                                                         {"gs", abi::kC0SsboGs}, {"ps", abi::kC0SsboPs},
                                                         {"ts", abi::kC0SsboTs}, {"ms", abi::kC0SsboMs}};
        std::stable_sort(ssbo.begin(), ssbo.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        for (const auto& [k, b] : ssbo)
            if (k != "ts" && k != "ms") fill0.emplace_back(b, b + abi::kC0Row * abi::kC0SsboSlots);
        fill2 = {{abi::kC2Tex, abi::kC2Septex},
                 {abi::kC2Img, abi::kC2Img + abi::kC2Row * abi::kC2ImgSlots},
                 {abi::kC2Septex, abi::kC2Samp},
                 {abi::kC2Samp, abi::kC2Samp + abi::kC2Row * abi::kC2SepSlots}};
    }
    Json bad = Json::MakeArray();
    for (const auto& [bank, d] : dr.object()) {
        std::vector<std::pair<Int, Int>> fill = bank == "0" ? fill0 : fill2;
        if (bank == "0" && stage == "ps") {
            fill.emplace_back(abi::kC0SampleTable, abi::kC0SampleTable + abi::kC0SampleBytes);
            fill.emplace_back(abi::kC0SampleInfo, abi::kC0SampleInfo + abi::kC0Row);
        }
        if (bank == "0" && (stage == "ts" || stage == "ms")) {
            const Int s = Ssbo(stage).value_or(0);  // ts and ms have one
            fill.emplace_back(s, s + abi::kC0Row * abi::kC0SsboSlots);
        }
        const Json& offsets = d.at("vec4_offsets");
        bool has_info = false;
        for (const Json& o : offsets.array()) has_info = has_info || o.as_int() == abi::kC0SampleInfo;
        if (d.at("dynamic").as_bool() && bank == "0" && !(stage == "ps" && has_info))
            bad.push_back(absl::StrCat("c", bank, " dynamic index"));
        for (const Json& oj : offsets.array()) {
            const Int o = oj.as_int();
            bool any = false;
            for (const auto& [a, b] : fill) any = any || (o < b && a < o + 16);
            if (!any) bad.push_back(absl::StrCat("c", bank, "+", py::Hex(o)));
        }
    }
    return bad;
}

}  // namespace nxmac::dmsh::reflect
