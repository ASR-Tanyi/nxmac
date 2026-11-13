// dmsh_rewrite.py: the rewrites of a translated shader's MSL before its compile. Ampere's HMMA (hmma_msl) is not
// ported: a Maxwell shader never has it.
#include <algorithm>
#include <map>
#include <set>

#include "abi.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "modules.h"
#include "re.h"

namespace nxmac::dmsh::rewrite {
namespace {

using py::Int;

// 1 << shift == value, Python's way: a shift past 62 gives a number no 64-bit value is
bool Pow2Is(std::string_view shift, const Json& value) {
    const Int k = py::ToInt(shift);
    return value.is_int() && k < 63 && (Int(1) << k) == value.as_int();
}

}  // namespace

std::string Precise(const std::string& msl) {
    static const re::Pattern p(R"(\bfast::(min|max|clamp)\()");
    return p.sub(R"(precise::\1()", msl);
}

std::string MulExtended(const std::string& msl) {
    static const re::Pattern helper(
        R"(template<typename T, typename U, typename V>\s*\[\[clang::optnone\]\] T spvMulExtended\(V l, V r\)\s*\{\s*return T\{U\(l \* r\), U\(mulhi\(l, r\)\)\};\s*\}\n)");
    std::vector<re::Match> found;
    if (py::Contains(msl, "spvMulExtended")) found = helper.finditer(msl);
    if (found.size() != 1 || py::Contains(msl, "spvMulExtended(uint l, uint r)")) return msl;
    std::string scalar;
    for (const auto& [t, w] : {std::pair{"uint", "ulong"}, std::pair{"int", "long"}}) {
        scalar += absl::StrFormat(
            "template<typename T, typename U> static inline __attribute__((always_inline)) T spvMulExtended(%s l, %s "
            "r) "
            "{ %s p = %s(l) * %s(r); return T{U(p), U(p >> 32)}; }\n",
            t, t, w, w, w);
    }
    const std::size_t at = found[0].end();
    return msl.substr(0, at) + scalar + msl.substr(at);
}

std::string HmmaMsl(const std::string& msl) {
    static const re::Pattern p(R"(\n(\w+) dmsh_hmma16816_f16\(([^)]*)\)\n\{)");
    if (!py::Contains(msl, "dmsh_hmma16816_f16") || !p.search(msl)) return msl;
    throw py::ValueError("dmsh_hmma16816_f16: Ampere's HMMA (NVN 2) is not ported to nxmac yet");
}

std::vector<std::string> SplitParams(std::string_view text) {
    std::vector<std::string> params;
    int depth = 0;
    std::string cur;
    const std::string all = absl::StrCat(text, ",");
    for (char ch : all) {
        depth += ch == '<' ? 1 : ch == '>' ? -1 : 0;
        if (ch == ',' && depth == 0) {
            params.emplace_back(py::Strip(cur));
            cur.clear();
        } else {
            cur += ch;
        }
    }
    return params;
}

Int BlockEnd(std::string_view src, Int start) {
    const Int n = static_cast<Int>(src.size());
    if (start < n && start >= -n) {
        const char c = src[static_cast<std::size_t>(start < 0 ? start + n : start)];
        if (c != '{' && c != '}') return start;
    }
    static const re::Pattern brace(R"([{}])");
    int depth = 0;
    for (const re::Match& b : brace.finditer(src, static_cast<std::size_t>(std::max<Int>(start, 0)))) {
        depth += b.group() == "{" ? 1 : -1;
        if (depth == 0) return static_cast<Int>(b.start());
    }
    throw py::ValueError("dmsh_hmma16816_f16: block not closed");
}

std::pair<std::string, std::vector<FirstStore>> FirstStores(const std::string& msl, const Json& local) {
    static const re::Pattern first(
        R"(float (\w+) = as_type<float>\(gl_WorkGroupID\.x\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.x\);\s*int (\w+) = as_type<int>\(\1\) << (\d+);\s*)"
        R"(int (\w+) = ~as_type<int>\(\2\);\s*int (\w+) = \5 \+ 1;\s*bool (\w+) = \6 != \3;\s*)");
    static const re::Pattern first2d(
        R"(float (\w+) = as_type<float>\(gl_WorkGroupID\.y\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.y\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.x\);\s*)"
        R"(float (\w+) = as_type<float>\(gl_WorkGroupID\.x\);\s*float (\w+) = as_type<float>\(gl_WorkGroupID\.z\);\s*)"
        R"(int (\w+) = as_type<int>\(\1\) << (\d+);\s*int (\w+) = ~as_type<int>\(\2\);\s*int (\w+) = \8 \+ 1;\s*bool (\w+) = \9 != \6;\s*)"
        R"(int (\w+) = as_type<int>\(\4\) << (\d+);\s*int (\w+) = ~as_type<int>\(\3\);\s*int (\w+) = \13 \+ 1;\s*bool (\w+) = \14 == \11;\s*)"
        R"(bool (\w+) = !\10;\s*bool (\w+) = \15 && \16;\s*bool (\w+) = (?:as_type<int>\(\5\) == 0|0 == as_type<int>\(\5\));\s*bool (\w+) = \18 && \17;\s*(?:bool (\w+) = !\19;\s*)?)");
    auto none = [&msl] { return std::pair<std::string, std::vector<FirstStore>>{msl, {}}; };
    // `not local or local[2:] != [1]`
    if (!local.truthy() || !local.is_array() || local.size() != 3 || !local.array()[2].is_int() ||
        local.array()[2].as_int() != 1)
        return none();
    const auto& l = local.array();
    std::string guard;
    std::optional<re::Match> m = first.search(msl);
    if (m) {
        if (!(l[1].is_int() && l[1].as_int() == 1) || !Pow2Is(m->group(4), l[0])) return none();
        guard = absl::StrCat("!", re::escape(m->group(7)));
    } else {
        m = first2d.search(msl);
        if (!m || !Pow2Is(m->group(7), l[1]) || !Pow2Is(m->group(12), l[0])) return none();
        guard = m->has(20) ? absl::StrCat("!", re::escape(m->group(20))) : re::escape(m->group(19));
    }
    const re::Pattern held(
        absl::StrFormat(R"(int (\w+) = \w+;\s*if \(%s\)\s*\{\s*\1 = (\d+);\s*\}\s*int (\w+) = \1;\s*)", guard));
    const re::Pattern block(absl::StrFormat(
        R"(if \(%s\)\s*\{\s*((?:\(\*\w+\.\w+\)\._m0\[\d+\] = (?:\d+u|uint\(\w+\));\s*)+)\}\s*)", guard));
    static const re::Pattern plain(R"(int \w+ = [^;{}]*;\s*)");
    static const re::Pattern store(R"(\(\*\w+\.(\w+)\)\._m0\[(\d+)\] = (\d+u|uint\(\w+\));)");
    std::size_t at = m->end();
    std::string out = msl.substr(0, at);
    std::vector<FirstStore> stores;
    std::map<std::string, Int> values;
    while (true) {
        if (auto h = held.match(msl, at)) {
            values[std::string(h->group(3))] = py::ToInt(h->group(2));
            out += msl.substr(at, h->end() - at);
            at = h->end();
            continue;
        }
        if (auto b = block.match(msl, at)) {
            for (const re::Match& s : store.finditer(b->group(1))) {
                const std::string v(s.group(3));
                if (py::EndsWith(v, "u")) {
                    stores.push_back(
                        {std::string(s.group(1)), py::ToInt(s.group(2)), py::ToInt(v.substr(0, v.size() - 1))});
                } else if (auto it = values.find(v.substr(5, v.size() - 6)); it != values.end()) {
                    stores.push_back({std::string(s.group(1)), py::ToInt(s.group(2)), it->second});
                } else {
                    return none();
                }
            }
            at = b->end();
            continue;
        }
        auto p = plain.match(msl, at);
        if (!p || held.match(msl, p->start())) break;
        out += msl.substr(at, p->end() - at);
        at = p->end();
    }
    const std::string rest = msl.substr(at);
    if (stores.empty() || stores.size() > 4 || re::search(absl::StrFormat(R"(if \(%s\))", guard), rest)) return none();
    return {out + rest, stores};
}

std::string SharedStoreMsl(const std::string& msl) {
    if (!py::Contains(msl, "dmsh_shared_store")) return msl;
    static const re::Pattern shared(R"(\nvoid (dmsh_shared_store(8|16)_\d+)\(([^)]*)\)\n\{)");
    static const re::Pattern scalar_p(R"(thread const u?int& (\w+))");
    static const re::Pattern arr_p(R"((threadgroup|device) spvUnsafeArray<uint, \d+>& (\w+))");
    const std::vector<re::Match> found = shared.finditer(msl);
    if (found.empty()) throw py::ValueError("dmsh_shared_store: no definition of the expected form");
    std::string out;
    std::size_t at = 0;
    bool any16 = false;
    for (const re::Match& m : found) {
        const std::vector<std::string> params = SplitParams(m.group(3));
        std::optional<re::Match> s0 = params.size() > 0 ? scalar_p.fullmatch(params[0]) : std::nullopt;
        std::optional<re::Match> s1 = params.size() > 1 ? scalar_p.fullmatch(params[1]) : std::nullopt;
        std::optional<re::Match> arr = params.size() == 3 ? arr_p.fullmatch(params[2]) : std::nullopt;
        if (!arr || !s0 || !s1)
            throw py::ValueError(absl::StrCat("dmsh_shared_store: unexpected parameters ", py::To(m.group(3), 160)));
        const std::string off(s0->group(1)), val(s1->group(1)), space(arr->group(1)), mem(arr->group(2));
        const bool is8 = m.group(2) == "8";
        any16 = any16 || !is8;
        const std::string st =
            is8 ? absl::StrFormat("((%s uchar*)&%s[0])[uint(%s)] = uchar(uint(%s));", space, mem, off, val)
                : absl::StrFormat("((%s dm_ushort_alias*)&%s[0])[uint(%s) >> 1u] = ushort(uint(%s));", space, mem, off,
                                  val);
        out += msl.substr(at, m.end() - at);
        out += "\n    " + st + "\n";
        at = static_cast<std::size_t>(BlockEnd(msl, static_cast<Int>(m.end()) - 1));
    }
    out += msl.substr(at);
    if (any16) {
        Int u = py::Find(out, "using namespace metal;\n");
        if (u < 0) throw py::ValueError("dmsh_shared_store: no using namespace metal");
        u += static_cast<Int>(std::string_view("using namespace metal;\n").size());
        out = out.substr(0, u) +
              "typedef ushort __attribute__((may_alias)) dm_ushort_alias;   // a half-word that may alias the shared "
              "memory's words (see shared_store_msl)\n" +
              out.substr(u);
    }
    return out;
}

std::pair<std::string, Int> TgSpill(const std::string& msl) {
    static const re::Pattern kernel(R"(\bkernel\s+void\s+main0\s*\()");
    static const re::Pattern decl_p(
        R"(^([ \t]*)threadgroup (spvUnsafeArray<(\w+), (\d+)>|\w+) (\w+)(?:\[(\d+)\])?;[ \t]*$)", re::M);
    static const std::map<std::string, Int> bytes = {
        {"uint", 4},   {"int", 4},     {"float", 4},  {"uint2", 8}, {"int2", 8},    {"float2", 8}, {"uint3", 16},
        {"int3", 16},  {"float3", 16}, {"uint4", 16}, {"int4", 16}, {"float4", 16}, {"half", 2},   {"short", 2},
        {"ushort", 2}, {"half2", 4},   {"half4", 8},  {"char", 1},  {"uchar", 1}};
    auto m = kernel.search(msl);
    if (!m) return {msl, 0};
    std::vector<std::pair<re::Match, Int>> decls;
    Int total = 0;
    for (re::Match& d : decl_p.finditer(msl, m->end())) {
        const std::string elem(d.has(3) ? d.group(3) : d.group(2));
        const Int n = d.has(4) ? py::ToInt(d.group(4)) : d.has(6) ? py::ToInt(d.group(6)) : 1;
        auto it = bytes.find(elem);
        if (it == bytes.end()) throw py::ValueError(absl::StrCat("threadgroup array of ", elem));
        decls.emplace_back(d, total);
        total += (it->second * n + 15) & ~Int(15);
    }
    if (total <= abi::kTgMax) return {msl, 0};
    int depth = 1;
    std::size_t i = m->end();
    while (depth) {
        depth += msl[i] == '(' ? 1 : msl[i] == ')' ? -1 : 0;
        ++i;
    }
    const std::size_t sig_end = i - 1;
    const std::string params = msl.substr(m->end(), sig_end - m->end());
    auto g = re::search(R"(uint3 (\w+) \[\[threadgroup_position_in_grid\]\])", params);
    const std::string gid = g ? std::string(g->group(1)) : "dm_tgid";
    const std::string add =
        absl::StrCat(g ? "" : ", uint3 dm_tgid [[threadgroup_position_in_grid]]",
                     ", uint3 dm_ntg [[threadgroups_per_grid]], device uchar* dm_tg [[buffer(", abi::kTgSlot, ")]]");
    std::string out;
    std::size_t at = 0;
    for (std::size_t k = 0; k < decls.size(); ++k) {
        const re::Match& d = decls[k].first;
        const Int off = decls[k].second;
        out += msl.substr(at, d.start() - at);
        const std::string t = absl::StrCat(d.group(2), d.has(6) ? absl::StrCat("[", d.group(6), "]") : "");
        if (k == 0) {
            out += absl::StrCat(d.group(1), "ulong dm_tgbase = ((ulong(", gid, ".z) * dm_ntg.y + ", gid,
                                ".y) * dm_ntg.x + ", gid, ".x) * ", total, "ul;\n");
        }
        if (d.has(6)) {
            out += absl::StrCat(d.group(1), "device ", d.group(2), " (&", d.group(5), ")[", d.group(6), "] = *(device ",
                                d.group(2), " (*)[", d.group(6), "])(dm_tg + dm_tgbase + ", off, ");");
        } else {
            out += absl::StrCat(d.group(1), "device ", t, "& ", d.group(5), " = *(device ", t,
                                "*)(dm_tg + dm_tgbase + ", off, ");");
        }
        at = d.end();
    }
    out += msl.substr(at);
    const Int e = py::Index(out, params, static_cast<Int>(m->start())) + static_cast<Int>(params.size());
    std::string src = out.substr(0, e) + add + out.substr(e);
    std::vector<std::string> lines = py::Split(src, "\n");
    static const re::Pattern tg(R"(\bthreadgroup\b(?=\s))");
    static const re::Pattern mem(R"((?<!mem_device \| )mem_flags::mem_threadgroup\b)");
    for (std::string& ln : lines) {
        if (py::Contains(ln, "operator [] (size_t pos)")) continue;
        ln = tg.sub("device", ln);
        ln = mem.sub("mem_flags::mem_device | mem_flags::mem_threadgroup", ln);
    }
    return {py::Join(lines, "\n"), total};
}

std::optional<std::pair<std::string, std::string>> GatherQuad(const std::vector<std::string>& off) {
    static const re::Pattern p(R"(int2\(\s*(-?\d+)\s*(?:,\s*(-?\d+)\s*)?\))");
    std::vector<std::pair<Int, Int>> xy;
    for (const std::string& o : off) {
        auto m = p.fullmatch(o);
        if (!m) return std::nullopt;
        xy.emplace_back(py::ToInt(m->group(1)), py::ToInt(m->has(2) ? m->group(2) : m->group(1)));
    }
    if (xy.empty()) return std::nullopt;  // min() of nothing raises in Python; four offsets always reach here
    Int bx = xy[0].first, by = xy[0].second;
    for (const auto& [x, y] : xy) {
        bx = std::min(bx, x);
        by = std::min(by, y);
    }
    std::vector<std::pair<Int, Int>> rel;
    rel.reserve(xy.size());
    for (const auto& [x, y] : xy) rel.emplace_back(x - bx, y - by);
    static const std::map<std::pair<Int, Int>, char> quad = {
        {{0, 0}, 'w'}, {{1, 0}, 'z'}, {{0, 1}, 'x'}, {{1, 1}, 'y'}};
    std::vector<std::pair<Int, Int>> sorted_rel = rel, keys;
    std::sort(sorted_rel.begin(), sorted_rel.end());
    for (const auto& [k, v] : quad) keys.push_back(k);
    if (sorted_rel != keys) return std::nullopt;
    std::string sw;
    for (const auto& r : rel) sw += quad.at(r);
    return std::pair{absl::StrFormat("int2(%d, %d)", bx, by), sw};
}

std::string GatherOffsets(const std::string& msl) {
    static const re::Pattern any(R"(\bspvGather(Compare)?ConstOffsets\(spvDesc|= spvGather(Compare)?ConstOffsets\()");
    if (!any.search(msl)) return msl;
    static const re::Pattern array_p(
        R"(\bconstant spvUnsafeArray<int2, 4> (\w+) = spvUnsafeArray<int2, 4>\(\{ (.*?) \}\);)");
    static const re::Pattern int2(R"(int2\([^()]*\))");
    static const re::Pattern call(R"(\bspvGather(Compare)?ConstOffsets\((?=[^)]*?\w))");
    std::map<std::string, std::vector<std::string>> arrays;
    for (const re::Match& m : array_p.finditer(msl)) {
        std::vector<std::string> off = int2.findall(m.group(2));
        if (off.size() != 4)
            throw py::ValueError(absl::StrCat("gather offsets: unexpected array ", py::To(m.group(0), 120)));
        arrays[std::string(m.group(1))] = off;
    }
    std::string out;
    std::size_t i = 0;
    for (const re::Match& m : call.finditer(msl)) {
        if (m.start() < i || py::Slice(msl, m.end(), m.end() + 6) == "const ") continue;
        int depth = 1;
        std::vector<std::string> args;
        std::string cur;
        std::size_t j = m.end();
        while (depth) {
            const char ch = msl.at(j);
            ++j;
            depth += ch == '(' ? 1 : ch == ')' ? -1 : 0;
            if (depth == 0 || (ch == ',' && depth == 1)) {
                args.emplace_back(py::Strip(cur));
                cur.clear();
            } else {
                cur += ch;
            }
        }
        const bool cmp = m.has(1);
        const std::size_t base = cmp ? 3 : 4;
        if (args.size() <= base || !arrays.count(args[2]))
            throw py::ValueError(absl::StrCat("gather offsets: unexpected call ",
                                              py::To(std::string_view(msl).substr(m.start(), j - m.start()), 160)));
        const std::string& tex = args[0];
        const std::string& smp = args[1];
        const std::vector<std::string>& off = arrays[args[2]];
        std::vector<std::string> rest_v(args.begin() + static_cast<std::ptrdiff_t>(base), args.end());
        const std::string rest = py::Join(rest_v, ", ");
        const std::string tail = cmp ? "" : ", " + args[3];
        const std::string fn = cmp ? "gather_compare" : "gather";
        out += msl.substr(i, m.start() - i);
        if (auto q = GatherQuad(off)) {
            out += absl::StrCat("(", tex, ".", fn, "(", smp, ", ", rest, ", ", q->first, tail, ").", q->second, ")");
        } else {
            std::vector<std::string> g;
            g.reserve(off.size());
            for (const std::string& o : off)
                g.push_back(absl::StrCat(tex, ".", fn, "(", smp, ", ", rest, ", ", o, tail, ").w"));
            out += "dm_gather4(" + py::Join(g, ", ") + ")";
        }
        i = j;
    }
    out += msl.substr(i);
    if (!py::Contains(out, "dm_gather4(")) return out;
    const Int k = py::Index(out, "\n", py::Index(out, "using namespace metal;")) + 1;
    return out.substr(0, k) +
           "template<typename T> static inline __attribute__((always_inline)) vec<T, 4> dm_gather4(T a, T b, T c, T d) "
           "{ return vec<T, 4>(a, b, c, d); }\n" +
           out.substr(k);
}

std::string BorderEmu(const std::string& msl_in) {
    static const re::Pattern body_p(R"(struct spvDescriptorSetBuffer0\s*\{(.*?)\};)", re::S);
    static const re::Pattern member_p(R"(^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)(?:\s*,\s*\w+\(\d+\))*\]\];)", re::M);
    static const re::Pattern ftex_p(R"((texture|depth)\w*<float(,.*)?>)");
    static const re::Pattern call(R"(spvDescriptorSet0\.(\w+)\.sample\(spvDescriptorSet0\.(\w+), )");
    static const re::Pattern arr_p(R"(array<.*,\s*(\d+)>)");
    auto body = body_p.search(msl_in);
    if (!body) return msl_in;
    struct Member {
        std::string type, name;
        Int id;
    };
    std::vector<Member> members;
    for (const re::Match& m : member_p.finditer(body->group(1)))
        members.push_back({std::string(py::Strip(m.group(1))), std::string(m.group(2)), py::ToInt(m.group(3))});
    std::vector<std::string> samplers;
    std::set<std::string> ftex;
    for (const Member& mb : members) {
        if (mb.type == "sampler") samplers.push_back(mb.name);
        if (ftex_p.fullmatch(mb.type)) ftex.insert(mb.name);
    }
    std::set<std::string> used;
    for (const re::Match& m : call.finditer(msl_in))
        if (ftex.count(std::string(m.group(1)))) used.insert(std::string(m.group(2)));
    std::vector<std::pair<std::string, int>> bit;  // in the samplers' order
    std::map<std::string, int> bit_of;
    for (const std::string& n : samplers) {
        if (!used.count(n) || static_cast<int>(bit.size()) >= abi::kBorderSamplers) continue;
        bit_of[n] = static_cast<int>(bit.size());
        bit.emplace_back(n, static_cast<int>(bit.size()));
    }
    if (bit.empty()) return msl_in;
    Int nid = INT64_MIN;
    for (const Member& mb : members) {
        auto a = arr_p.fullmatch(mb.type);
        nid = std::max(nid, mb.id + (a ? py::ToInt(a->group(1)) : 1));
    }
    std::string add;
    for (const auto& [n, i] : bit)
        add += absl::StrCat("    sampler ", n, abi::kBorderTwin, " [[id(", nid + i, ")]];\n");
    add += absl::StrCat("    constant dm_border_t* ", abi::kBorderMember, " [[id(", nid + static_cast<Int>(bit.size()),
                        ")]];\n");
    const std::size_t end = body->end(1);
    std::string msl = msl_in.substr(0, end) + (msl_in[end - 1] == '\n' ? "" : "\n") + add + msl_in.substr(end);
    struct Call {
        std::size_t start, end;
        std::string texture, sampler;
    };
    std::vector<Call>
        calls;  // of the text before the edits, each at or after the previous one's start: made from the last back
    for (const re::Match& m : call.finditer(msl)) {
        if (bit_of.count(std::string(m.group(2))) && ftex.count(std::string(m.group(1))))
            calls.push_back({m.start(), m.end(), std::string(m.group(1)), std::string(m.group(2))});
    }
    for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
        const Call& m = *it;
        int depth = 1;
        std::size_t i = m.end;
        while (depth) {
            depth += msl.at(i) == '(' ? 1 : msl[i] == ')' ? -1 : 0;
            ++i;
        }
        const std::string args = msl.substr(m.end, i - 1 - m.end);
        const std::string twin = absl::StrCat("spvDescriptorSet0.", m.sampler, abi::kBorderTwin);
        msl = absl::StrCat(msl.substr(0, m.start), "dm_bs(", msl.substr(m.start, i - m.start), ", spvDescriptorSet0.",
                           abi::kBorderMember, ", ", bit_of[m.sampler], "u, spvDescriptorSet0.", m.texture, ", ", twin,
                           ", ", args, ")", msl.substr(i));
    }
    const std::string helpers = absl::StrFormat(
        "\nstruct dm_border_t { uint mask; float4 k[%d]; };\n"
        "static inline __attribute__((always_inline)) float4 dm_bmix(float4 r, float4 w, float4 k) { return r + (w - "
        "r) * k; }\n"
        "static inline __attribute__((always_inline)) float dm_bmix(float r, float w, float4 k) { return r + (w - r) * "
        "k.x; }\n"
        "template<typename V, typename T, typename... A> static inline __attribute__((always_inline)) V dm_bs(V r, "
        "constant dm_border_t* b, uint i, T t, sampler w, A... a)\n"
        "{\n"
        "    if (b->mask >> i & 1u) r = dm_bmix(r, t.sample(w, a...), b->k[i]);\n"
        "    return r;\n"
        "}\n",
        abi::kBorderSamplers);
    return py::Replace(msl, "using namespace metal;\n", absl::StrCat("using namespace metal;\n", helpers), 1);
}

std::pair<std::string, int> FragZSnap(const std::string& msl) {
    if (!py::Contains(msl, "gl_FragCoord.z")) return {msl, 0};
    const std::optional<side::BodySpan> shared = side::Body(msl);
    std::optional<side::Entry> e;
    if (!shared) {
        try {
            e = side::FindEntry(msl, "main0");
        } catch (const py::ValueError&) {
            return {msl, 0};
        }
    }
    Int head, end;
    std::vector<std::string> params;
    if (shared) {
        head = shared->head;
        end = shared->end;
        static const re::Pattern comma(R"([^,]+)");
        params = comma.findall(py::Slice(msl, shared->start, head));
    } else if (e) {
        head = e->head;
        end = e->end;
        params = e->params;
    } else {
        return {msl, 0};
    }
    static const re::Pattern frag(R"(\bfloat4 gl_FragCoord( \[\[position\]\])?\)?$)");
    bool any = false;
    for (const std::string& p : params) any = any || frag.search(py::Strip(p)).has_value();
    if (!any) return {msl, 0};
    const Int first = py::Find(msl, "{", head) + 1;
    std::vector<std::string> lines = py::Split(py::Slice(msl, first, end - 1), "\n");
    static const re::Pattern decl(R"(\s*(?:const )?(float|bool|int|uint)\s+(_\d+) = (.+);$)");
    static const re::Pattern ref(R"(\b_\d+\b)");
    static const re::Pattern texread(R"(\.(sample|read)\()");
    static const re::Pattern cmp(R"((_\d+) (?:<=|>=|<|>|==|!=) (_\d+))");
    static const re::Pattern brace(R"([{}])");
    std::map<std::string, std::set<std::string>> zr, xr;
    std::map<std::string, std::vector<int>> scope;
    std::map<std::string, std::size_t> line_of;
    std::vector<int> stack{0};
    int blocks = 0;
    std::map<std::string, std::set<std::string>> pairs;
    std::vector<std::string> pairs_order;  // dict insertion order of pairs
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& ln = lines[i];
        if (auto m = decl.match(ln)) {
            const std::string ty(m->group(1)), v(m->group(2)), ex(m->group(3));
            std::set<std::string> z, x;
            for (const std::string& r : ref.findall(ex)) {
                if (auto it = zr.find(r); it != zr.end()) z.insert(it->second.begin(), it->second.end());
                if (auto it = xr.find(r); it != xr.end()) x.insert(it->second.begin(), it->second.end());
            }
            if (ex == "gl_FragCoord.z") {
                z = {v};
            } else if (ty == "float" && texread.search(ex) && z.empty()) {
                x = {v};
            }
            zr[v] = z;
            xr[v] = x;
            scope[v] = stack;
            line_of[v] = i;
            std::optional<re::Match> c;
            if (ty == "bool") c = cmp.fullmatch(ex);
            if (c) {
                const std::string a(c->group(1)), b(c->group(2));
                for (const auto& [zs, xs] : {std::pair{a, b}, std::pair{b, a}}) {
                    const bool zz = zr.count(zs) && !zr[zs].empty();
                    const bool xx = xr.count(xs) && !xr[xs].empty();
                    const bool zx = xr.count(zs) && !xr[zs].empty();
                    const bool xz = zr.count(xs) && !zr[xs].empty();
                    if (zz && xx && !zx && !xz) {
                        for (const std::string& zv : zr[zs]) {
                            if (!pairs.count(zv)) pairs_order.push_back(zv);
                            pairs[zv].insert(xr[xs].begin(), xr[xs].end());
                        }
                    }
                }
            }
        }
        for (const std::string& ch : brace.findall(ln)) {
            if (ch == "{") {
                ++blocks;
                stack.push_back(blocks);
            } else if (stack.size() > 1) {
                stack.pop_back();
            }
        }
    }
    int n = 0;
    std::vector<std::string> zs = pairs_order;
    std::stable_sort(zs.begin(), zs.end(),
                     [&](const std::string& a, const std::string& b) { return line_of[a] < line_of[b]; });
    // sorted(pairs, key=line_of): pairs' keys sorted by line, ties in dict order (stable); a set's names have no order
    // in Python but each of them is declared once, so lines differ
    for (const std::string& zv : zs) {
        std::vector<std::string> xs(pairs[zv].begin(), pairs[zv].end());
        std::stable_sort(xs.begin(), xs.end(),
                         [&](const std::string& a, const std::string& b) { return line_of[a] < line_of[b]; });
        std::vector<std::string> keep;
        for (const std::string& x : xs) {
            const std::vector<int>& sx = scope[x];
            const std::vector<int>& sz = scope[zv];
            const bool sees = sz.size() >= sx.size() ? std::equal(sx.begin(), sx.end(), sz.begin()) : false;
            if (line_of[x] < line_of[zv] && sees) keep.push_back(x);
        }
        if (keep.empty()) continue;
        std::string expr = "gl_FragCoord.z";
        for (const std::string& x : keep) expr = absl::StrCat("dm_zsnap(", expr, ", ", x, ", dm_zw)");
        std::string& line = lines[line_of[zv]];
        line = py::Replace(line, absl::StrCat(zv, " = gl_FragCoord.z;"), absl::StrCat(zv, " = ", expr, ";"));
        ++n;
    }
    if (!n) return {msl, 0};
    const std::string body = "\n    float dm_zw = fwidth(gl_FragCoord.z);" + py::Join(lines, "\n");
    const std::string out = msl.substr(0, first) + body + msl.substr(end - 1);
    return {py::Replace(out, "using namespace metal;\n",
                        "using namespace metal;\n\n"
                        "static inline __attribute__((always_inline)) float dm_zsnap(float z, float d, float zw)   // "
                        "see frag_z_snap\n"
                        "{\n    float lo = as_type<float>(as_type<int>(z) - 1) - zw * 0x1p-20f;\n"
                        "    return (z > 0.0f && d < z && d >= lo) ? d : z;\n}\n",
                        1),
            n};
}

std::pair<std::string, int> BlendZero(const std::string& msl) {
    static const re::Pattern st_p(R"(struct main0_out\s*\{(.*?)\};)", re::S);
    static const re::Pattern col_p(R"(^\s*(\w+)\s+(\w+)\s*\[\[color\((\d+)\)\]\];)", re::M);
    auto st = st_p.search(msl);
    std::vector<std::pair<std::string, Int>> cols;
    if (st) {
        for (const re::Match& m : col_p.finditer(st->group(1)))
            if (m.group(1) == "float4") cols.emplace_back(std::string(m.group(2)), py::ToInt(m.group(3)));
    }
    if (cols.empty() || py::Contains(msl, "dm_blend0(") ||
        py::Contains(msl, absl::StrCat("[[buffer(", abi::kBlendBuffer, ")]]")) || side::Body(msl))
        return {msl, 0};
    std::optional<side::Entry> e;
    try {
        e = side::FindEntry(msl, "main0");
    } catch (const py::ValueError&) {
        return {msl, 0};
    }
    if (!e || e->ret != "main0_out") return {msl, 0};
    std::string body = msl.substr(e->head, e->end - e->head);
    static const re::Pattern ret(R"(\breturn\b)");
    static const re::Pattern ret_out(R"(^([ \t]*)return out;$)", re::M);
    const std::size_t n = ret.finditer(body).size();
    std::size_t k = 0;
    body = ret_out.sub(
        [](const re::Match& m) {
            return absl::StrCat(m.group(1), "dm_blend0(out, dm_blend);\n", m.group(1), "return out;");
        },
        body, 0, &k);
    if (!k || k != n) return {msl, 0};
    const int bits = abi::kBlendZeroBits;
    std::string per;
    for (const auto& [nm, c] : cols)
        per += absl::StrFormat("    dm_o.%s = dm_bz(dm_o.%s, m >> %du);\n", nm, nm, bits * c);
    const std::string helper = absl::StrFormat(
        "struct dm_blend_state { uint zero; };   // see dmsh_rewrite.py blend_zero\n"
        "static inline __attribute__((always_inline)) float4 dm_bz(float4 c, uint m)\n"
        "{\n"
        "    m &= %du;\n"
        "    if (!((m == 1u && c.w == 0.0f) || (m == 2u && c.w == 1.0f))) return c;\n"
        "    return float4(select(c.xyz, float3(0.0f), (as_type<uint3>(c.xyz) & uint3(0x7f800000u)) == "
        "uint3(0x7f800000u)), c.w);   // NaN or Inf by its bits: fast math may assume there is none\n"
        "}\n"
        "static inline __attribute__((always_inline)) void dm_blend0(main0_out& dm_o, constant dm_blend_state& dm_s)\n"
        "{\n"
        "    uint m = dm_s.zero;\n"
        "    if (m == 0u) return;\n"
        "%s}\n",
        (1 << bits) - 1, per);
    const std::string param = absl::StrCat("constant dm_blend_state& dm_blend [[buffer(", abi::kBlendBuffer, ")]]");
    return {absl::StrCat(msl.substr(0, e->start), helper, "\n", msl.substr(e->start, e->head - 1 - e->start),
                         e->params.empty() ? "" : ", ", param, ")", body, msl.substr(e->end)),
            static_cast<int>(k)};
}

}  // namespace nxmac::dmsh::rewrite
