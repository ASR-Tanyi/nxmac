// dmsh_atomic.py, dmsh_wide.py, dmsh_vin.py, dmsh_pull.py, the part of dmsh_geometry.py a vertex shader's build takes,
// and dmsh_build.py's SPIR-V rewrite for one-lane subgroups.
#include <cstring>
#include <map>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "modules.h"
#include "re.h"

namespace nxmac::dmsh {
namespace {

using py::Int;

std::vector<std::uint32_t> Words(std::string_view data) {
    std::vector<std::uint32_t> w(data.size() / 4);
    if (!w.empty()) std::memcpy(w.data(), data.data(), w.size() * 4);
    return w;
}

}  // namespace

// ---- dmsh_atomic.py

bool atomic::UsesImageAtomics(std::string_view data, std::string_view path) {
    if (data.size() < 20 || data.substr(0, 4) != std::string_view("\x03\x02\x23\x07", 4))
        throw py::Fatal(absl::StrCat("not a SPIR-V module: ", path));
    if (!absl::StrContains(data, std::string_view("\x3c\x00\x06\x00", 4))) return false;
    const std::vector<std::uint32_t> words = Words(data);
    std::size_t i = 5;
    while (i < words.size()) {
        const std::uint32_t n = words[i] >> 16, op = words[i] & 0xffff;
        if (op == 60) return true;
        if (n == 0) throw py::Fatal(absl::StrCat("malformed SPIR-V instruction in ", path));
        i += n;
    }
    return false;
}

std::vector<std::string> atomic::CrossArgs(const std::vector<std::string>& xc, std::string_view spv,
                                           std::string_view path) {
    std::vector<std::string> args = xc;
    if (!UsesImageAtomics(spv, path)) return args;
    for (std::size_t k = 0; k + 1 < args.size(); ++k) {
        if (args[k] == "--msl-version") {
            args[k + 1] = kMslVersionAtomics;
            break;
        }
    }
    return args;
}

void atomic::CheckMsl(const std::string& source) {
    static const re::Pattern emulated(R"(\bspvImage2DAtomicCoord\b|device\s+atomic_u?int\s*\*\s*\w+_atomic\b)");
    if (py::Contains(source, "tomic") && emulated.search(source))
        throw py::ValueError(
            "image atomics emulated through a raw pointer; they must be native texture atomics (MSL 3.1)");
}

// ---- dmsh_wide.py

std::string wide::WideLoads(const std::string& msl) {
    if (!py::Contains(msl, " >> uint(2)")) return msl;
    static const std::string ident = R"(_[0-9A-Za-z_]+)";
    static const std::string buf = R"([a-z]p_s\d+)";
    static const re::Pattern add_p(absl::StrCat(R"(^\s*int ()", ident, R"() = ()", ident, R"() \+ (-?\d+);\s*$)"));
    static const re::Pattern idx_p(
        absl::StrCat(R"(^\s*uint ()", ident, R"() = uint\(int\(uint\(()", ident, R"()\) >> uint\(2\)\)\);\s*$)"));
    static const re::Pattern load_p(
        absl::StrCat(R"(\(\*spvDescriptorSet0\.()", buf, R"()\)\._m0\[int\(()", ident, R"()\)\])"));
    static const re::Pattern ro_p(
        absl::StrCat(R"(^\s*const device )", ident, R"(\* ()", buf, R"() \[\[id\(\d+\)\]\];)"), re::M);
    static const std::string op = R"((?:[-+*/%|&^]|<<|>>)?=(?!=))";
    static const re::Pattern write_p(
        absl::StrCat(R"(atomic_|_barrier\(|\.write\(|\]\s*)", op, R"(|^\s*\*|^\s*)", ident, R"(\s*)", op));
    std::set<std::string> ro;
    for (const std::string& r : ro_p.findall(msl)) ro.insert(r);
    if (ro.empty()) return msl;
    std::vector<std::string> lines = py::Split(msl, "\n");
    std::map<std::string, std::pair<std::string, Int>> adds;
    std::map<std::string, std::string> idx;
    for (const std::string& ln : lines) {
        if (auto m = add_p.match(ln)) {
            adds[std::string(m->group(1))] = {std::string(m->group(2)), py::ToInt(m->group(3))};
            continue;
        }
        if (auto m = idx_p.match(ln)) idx[std::string(m->group(1))] = m->group(2);
    }
    auto root = [&](std::string v) {
        Int off = 0;
        for (int k = 0; k < 64; ++k) {
            auto it = adds.find(v);
            if (it == adds.end()) break;
            off += it->second.second;
            v = it->second.first;
        }
        return std::pair{v, off};
    };
    struct Word {
        std::size_t line;
        std::size_t start, end;
        std::string index;
    };
    // (block, buffer, value) → {byte offset: word}, in the order the keys came (a dict's)
    std::vector<std::tuple<int, std::string, std::string>> keys;
    std::map<std::tuple<int, std::string, std::string>, std::map<Int, Word>> words;
    int block = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& ln = lines[i];
        if (py::Contains(ln, "{") || py::Contains(ln, "}") || write_p.search(ln)) {
            ++block;
            continue;
        }
        const std::vector<re::Match> ms = load_p.finditer(ln);
        if (ms.size() != 1 || !ro.count(std::string(ms[0].group(1))) || !idx.count(std::string(ms[0].group(2))))
            continue;
        const auto [v, off] = root(idx[std::string(ms[0].group(2))]);
        if (((off % 4) + 4) % 4 == 0) {
            const auto key = std::make_tuple(block, std::string(ms[0].group(1)), v);
            if (!words.count(key)) keys.push_back(key);
            words[key].emplace(off, Word{i, ms[0].start(), ms[0].end(), std::string(ms[0].group(2))});
        }
    }
    struct Edit {
        std::size_t start, end;
        std::string rep;
        std::optional<std::string> decl;
    };
    std::map<std::size_t, std::vector<Edit>> edits;
    std::vector<std::size_t> edit_order;
    int n = 0;
    static const char* vec[] = {"", "", "packed_uint2", "packed_uint3", "packed_uint4"};
    for (const auto& key : keys) {
        const std::string& b = std::get<1>(key);
        const std::map<Int, Word>& by_off = words[key];
        std::set<Int> bases;
        for (const auto& [off, w] : by_off) bases.insert(off - (((off % 16) + 16) % 16));
        for (Int base : bases) {
            std::vector<const Word*> run;
            while (run.size() < 4 && by_off.count(base + 4 * static_cast<Int>(run.size())))
                run.push_back(&by_off.at(base + 4 * static_cast<Int>(run.size())));
            bool before = false;
            for (const Word* w : run) before = before || w->line < run[0]->line;
            if (run.size() < 2 || before) continue;
            const std::string name = absl::StrCat("_dmw", n), v = vec[run.size()];
            ++n;
            const std::string decl = absl::StrFormat(
                "%s %s = *(const device %s*)&(*spvDescriptorSet0.%s)._m0[int(%s)];", v, name, v, b, run[0]->index);
            for (std::size_t w = 0; w < run.size(); ++w) {
                if (!edits.count(run[w]->line)) edit_order.push_back(run[w]->line);
                edits[run[w]->line].push_back({run[w]->start, run[w]->end,
                                               absl::StrCat(name, ".", std::string(1, "xyzw"[w])),
                                               w == 0 ? std::optional<std::string>(decl) : std::nullopt});
            }
        }
    }
    for (std::size_t i : edit_order) {
        std::vector<Edit> es = edits[i];
        // sorted(es, reverse=True): by start, then end, then the text, then the declaration (None first), largest first
        std::sort(es.begin(), es.end(), [](const Edit& a, const Edit& b) {
            if (a.start != b.start) return a.start > b.start;
            if (a.end != b.end) return a.end > b.end;
            if (a.rep != b.rep) return a.rep > b.rep;
            return a.decl.value_or("") > b.decl.value_or("");
        });
        std::string ln = lines[i];
        std::vector<std::string> pre;
        for (const Edit& e : es) {
            ln = ln.substr(0, e.start) + e.rep + ln.substr(e.end);
            if (e.decl) pre.push_back(*e.decl);
        }
        const std::string indent = lines[i].substr(0, lines[i].size() - py::LStrip(lines[i]).size());
        std::string out;
        for (const std::string& d : pre) out += indent + d + "\n";
        lines[i] = out + ln;
    }
    return py::Join(lines, "\n");
}

// ---- dmsh_vin.py

std::pair<std::optional<std::string>, Json> vin::Variant(const std::string& msl) {
    static const re::Pattern in_p(R"(\nstruct main0_in\n\{\n(.*?)\n\};)", re::S);
    static const re::Pattern decl(R"(    (float4|uint4|int4) (m_\d+(?:_\d+)*) \[\[attribute\((\d+)\)\]\];)");
    auto none = [](std::string_view why) { return std::pair{std::optional<std::string>(), Json(why)}; };
    auto m = in_p.search(msl);
    if (!m) return none(py::Contains(msl, "[[stage_in]]") ? "no main0_in struct" : "no integer input");
    struct Member {
        std::string t, n;
        Int l;
    };
    std::vector<Member> members;
    for (const std::string& ln : py::Split(m->group(1), "\n")) {
        auto mm = decl.fullmatch(ln);
        if (!mm) return none(absl::StrCat("an input declaration not understood: ", py::To(py::Strip(ln), 60)));
        members.push_back({std::string(mm->group(1)), std::string(mm->group(2)), py::ToInt(mm->group(3))});
    }
    std::vector<Member> ints;
    for (const Member& x : members)
        if (x.t != "float4") ints.push_back(x);
    if (ints.empty()) return none("no integer input");
    for (const Member& x : members)
        if (x.l >= 16) return none("an input at attribute 16 or above");
    Int slot = 16;
    Json shadows = Json::MakeArray();
    std::vector<std::string> extra;
    for (const Member& x : ints) {
        if (slot >= 31) break;
        extra.push_back(absl::StrFormat("    float4 %s_vf [[attribute(%d)]];", x.n, slot));
        Json e = Json::MakeObject();
        e.set("location", x.l);
        e.set("float", slot);
        shadows.push_back(e);
        ++slot;
    }
    for (std::size_t k = 0; k < shadows.size() && k < ints.size(); ++k) {
        if (slot >= 31) break;
        const Member& x = ints[k];
        const std::string o = x.t == "uint4" ? "int4" : "uint4";
        extra.push_back(absl::StrFormat("    %s %s_vo [[attribute(%d)]];", o, x.n, slot));
        shadows.array()[k].set(o == "int4" ? "sint" : "uint", slot);
        ++slot;
    }
    std::map<Int, const Json*> covered;
    for (const Json& e : shadows.array()) covered[e.at("location").as_int()] = &e;
    const std::string head = absl::StrCat(msl.substr(0, m->end()), "\n\nstruct main0_in_v\n{\n", m->group(1), "\n",
                                          py::Join(extra, "\n"), "\n};");
    std::string body = msl.substr(m->end());
    const Int cut = py::Find(body, "\nvertex void main0_tf(");
    if (cut >= 0) body = body.substr(0, cut) + "\n";
    std::string pro = "    main0_in in = {};\n";
    for (const Member& x : members) {
        std::string ors;
        if (auto it = covered.find(x.l); it != covered.end()) {
            ors = absl::StrFormat(" | as_type<%s>(dm_in.%s_vf)", x.t, x.n);
            if (it->second->contains("sint") || it->second->contains("uint"))
                ors += absl::StrFormat(" | as_type<%s>(dm_in.%s_vo)", x.t, x.n);
        }
        pro += absl::StrFormat("    in.%s = dm_in.%s%s;\n", x.n, x.n, ors);
    }
    for (const char* entry : {"main0", "main0_z"}) {
        auto hdr = re::search(
            absl::StrFormat(R"(\nvertex main0_out %s\(main0_in in \[\[stage_in\]\]([^\n]*)\)\n\{\n)", entry), body);
        if (!hdr) {
            if (std::string_view(entry) == "main0") return none("main0 signature not recognized");
            continue;
        }
        const std::string nw =
            absl::StrFormat("\nvertex main0_out %s(main0_in_v dm_in [[stage_in]]%s)\n{\n", entry, hdr->group(1)) + pro;
        body = absl::StrCat(body.substr(0, hdr->start()), nw, body.substr(hdr->end()));
    }
    return {head + body, shadows};
}

// ---- dmsh_pull.py

std::string pull::DropEntries(const std::string& msl, const std::set<std::string>& keep) {
    static const re::Pattern vertex(R"(vertex \S+ (\w+)\()");
    const std::vector<std::string> lines = py::Split(msl, "\n");
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < lines.size()) {
        auto m = vertex.match(lines[i]);
        if (m && !keep.count(std::string(m->group(1)))) {
            std::size_t j = i;
            while (j < lines.size() && lines[j] != "}") ++j;
            i = j + 1;
            continue;
        }
        out.push_back(lines[i]);
        ++i;
    }
    return py::Join(out, "\n");
}

std::pair<std::optional<std::string>, Json> pull::Variant(const std::string& msl) {
    static const re::Pattern in_p(R"(\nstruct main0_in\n\{\n(.*?)\n\};)", re::S);
    static const re::Pattern decl(R"(    (float4|uint4|int4) (m_\d+(?:_\d+)*) \[\[attribute\((\d+)\)\]\];)");
    auto none = [](std::string_view why) { return std::pair{std::optional<std::string>(), Json(why)}; };
    auto m = in_p.search(msl);
    if (!m) return none("no stage input");
    if (py::Contains(msl, "[[buffer(16)]]")) return none("buffer 16 is taken");
    struct Member {
        std::string t, n;
        Int l;
    };
    std::vector<Member> members;
    for (const std::string& ln : py::Split(m->group(1), "\n")) {
        auto mm = decl.fullmatch(ln);
        if (!mm) return none(absl::StrCat("an input declaration not understood: ", py::To(py::Strip(ln), 60)));
        members.push_back({std::string(mm->group(1)), std::string(mm->group(2)), py::ToInt(mm->group(3))});
    }
    for (const Member& x : members)
        if (x.l >= 16) return none("an input at attribute 16 or above");
    std::vector<std::string> decls;
    decls.reserve(members.size());
    for (const Member& x : members) decls.push_back(absl::StrFormat("    %s %s;", x.t, x.n));
    std::string head = absl::StrCat(msl.substr(0, m->start()), "\nstruct main0_in\n{\n", py::Join(decls, "\n"), "\n};");
    std::string body = DropEntries(msl.substr(m->end()), {"main0", "main0_z"});
    bool has_z = false;
    for (const char* entry : {"main0", "main0_z"}) {
        auto hdr = re::search(
            absl::StrFormat(R"(\nvertex main0_out %s\(main0_in in \[\[stage_in\]\]((?:, [^\n]*)?)\)\n\{\n)", entry),
            body);
        if (!hdr) {
            if (std::string_view(entry) == "main0") return none("main0 signature not recognized");
            continue;
        }
        const std::string rest(hdr->group(1));
        auto vid = re::search(R"(uint (\w+) \[\[vertex_id\]\])", rest);
        auto iid = re::search(R"(uint (\w+) \[\[instance_id\]\])", rest);
        auto bi = re::search(R"(uint (\w+) \[\[base_instance\]\])", rest);
        std::vector<std::string> params;
        if (!rest.empty()) params = py::Split(rest.substr(2), ", ");
        params.emplace_back("constant dm_vlayout& dm_lay [[buffer(16)]]");
        if (!vid) params.emplace_back("uint dm_vid [[vertex_id]]");
        if (!iid) params.emplace_back("uint dm_iid [[instance_id]]");
        if (!bi) params.emplace_back("uint dm_bi [[base_instance]]");
        const std::string vn = vid ? std::string(vid->group(1)) : "dm_vid";
        const std::string inn = iid ? std::string(iid->group(1)) : "dm_iid";
        const std::string bn = bi ? std::string(bi->group(1)) : "dm_bi";
        std::string pro = "    main0_in in;\n";
        for (const Member& x : members)
            pro += absl::StrFormat("    in.%s = as_type<%s>(dm_fetch(dm_lay, %du, %s, %s, %s));\n", x.n, x.t, x.l, vn,
                                   inn, bn);
        body = absl::StrCat(body.substr(0, hdr->start()), "\nvertex main0_out ", entry, "(", py::Join(params, ", "),
                            ")\n{\n", pro, body.substr(hdr->end()));
        if (std::string_view(entry) == "main0_z") has_z = true;
    }
    Int ui = py::Find(head, "using namespace metal;");
    if (ui < 0) return none("no \"using namespace metal;\"");
    ui += static_cast<Int>(std::string_view("using namespace metal;").size());
    head = absl::StrCat(head.substr(0, ui), "\n\n", kFetch, head.substr(ui));
    Json info = Json::MakeObject();
    info.set("entry", "main0");
    info.set("entry_z", has_z ? Json("main0_z") : Json());
    info.set("slot", 16);
    Json inputs = Json::MakeArray();
    for (const Member& x : members) inputs.push_back(x.l);
    info.set("inputs", inputs);
    return {head + body, info};
}

// ---- dmsh_geometry.py

std::optional<int> geometry::Helper(std::string_view name) {
    static const std::map<std::string, int, std::less<>> helpers = {
        {"vertex_output", 0}, {"geometry_vb_output", 1}, {"geometry_ib_output", 2},
        {"vb_info", 3},       {"trb_data", 4},           {"vb_input", 1}};
    auto it = helpers.find(name);
    if (it == helpers.end()) return std::nullopt;
    return it->second;
}

std::vector<std::pair<int, std::string>> geometry::OutputWords(const std::string& src) {
    static const re::Pattern st_p(R"(struct main0_out\s*\{(.*?)\};)", re::S);
    static const re::Pattern user(R"((float4|uint4|int4) (\w+) \[\[user\(locn(\d+)\)\]\];)");
    static const re::Pattern position(R"(float4 (\w+) \[\[position(?:, invariant)?\]\];)");
    static const re::Pattern fixed(
        R"((float|uint|int) (\w+) \[\[(point_size|render_target_array_index|viewport_array_index)\]\];)");
    static const re::Pattern clip(R"(float (\w+)(?:\[(\d+)\])? \[\[clip_distance\]\](?: \[(\d+)\])?;)");
    static const re::Pattern uclip(R"(float (\w+) \[\[user\(clip(\d)\)\]\];)");
    auto st = st_p.search(src);
    if (!st) throw py::ValueError("geometry capture: missing output struct");
    std::vector<std::pair<int, std::string>> stores;
    for (const std::string& line0 : py::SplitLines(st->group(1))) {
        const std::string line(py::Strip(line0));
        if (line.empty()) continue;
        if (auto v = user.fullmatch(line)) {
            const Int l = py::ToInt(v->group(3));
            if (l >= 32) throw py::ValueError("geometry capture: attribute outside Maxwell map");
            for (int c = 0; c < 4; ++c)
                stores.emplace_back(16 + static_cast<int>(l) * 4 + c,
                                    absl::StrCat("out.", v->group(2), ".", std::string(1, "xyzw"[c])));
            continue;
        }
        if (auto v = position.fullmatch(line)) {
            for (int c = 0; c < 4; ++c)
                stores.emplace_back(c, absl::StrCat("out.", v->group(1), ".", std::string(1, "xyzw"[c])));
            continue;
        }
        if (auto v = fixed.fullmatch(line)) {
            const int offset = v->group(3) == "point_size" ? 4 : v->group(3) == "render_target_array_index" ? 13 : 14;
            stores.emplace_back(offset, absl::StrCat("out.", v->group(2)));
            continue;
        }
        if (auto v = clip.fullmatch(line)) {
            const std::string_view n = v->has(2) && !v->group(2).empty() ? v->group(2)
                                       : v->has(3)                       ? v->group(3)
                                                                         : std::string_view();
            if (!n.empty() && py::ToInt(n) <= 8) {
                for (Int c = 0; c < py::ToInt(n); ++c)
                    stores.emplace_back(5 + static_cast<int>(c), absl::StrCat("out.", v->group(1), "[", c, "]"));
                continue;
            }
        }
        if (auto v = uclip.fullmatch(line)) {
            stores.emplace_back(5 + static_cast<int>(py::ToInt(v->group(2))), absl::StrCat("out.", v->group(1)));
            continue;
        }
        throw py::ValueError(absl::StrCat("geometry capture: unknown output ", line));
    }
    // dict(stores): a word's first place, its last expression
    std::vector<std::pair<int, std::string>> out;
    for (const auto& [k, e] : stores) {
        bool found = false;
        for (auto& [k2, e2] : out) {
            if (k2 == k) {
                e2 = e;
                found = true;
            }
        }
        if (!found) out.emplace_back(k, e);
    }
    return out;
}

std::string geometry::Capture(const std::string& src) {
    static const re::Pattern main0(R"(\bvertex main0_out main0\((.*?)\)\s*\{)", re::S);
    auto m = main0.search(src);
    if (!m) throw py::ValueError("geometry capture: missing main0 vertex entry");
    std::string params(m->group(1));
    const std::string body = src.substr(m->end());
    if (!py::EndsWith(py::RStrip(body), "}")) throw py::ValueError("geometry capture: unexpected function ending");
    std::string pro;
    for (const auto& [semantic, name] : {std::pair{"vertex_id", "dm_vid"}, std::pair{"instance_id", "dm_iid"}}) {
        auto found = re::search(absl::StrCat(R"(uint (\w+) \[\[)", semantic, R"(\]\])"), params);
        if (found) {
            pro += absl::StrFormat("    uint %s = %s;\n", name, found->group(1));
        } else {
            params += absl::StrCat(params.empty() ? "" : ", ", "uint ", name, " [[", semantic, "]]");
        }
    }
    const std::vector<std::pair<int, std::string>> stores = OutputWords(src);
    params += absl::StrFormat(", constant dm_geometry_capture& dm_gs [[buffer(%d)]]", kCaptureSlot);
    pro +=
        "    uint4 dm_ci = dm_gs.info; device uint* dm_capture = dm_gs.dst;\n"
        "    if (dm_vid - dm_ci.x >= dm_ci.y || dm_iid - dm_ci.z >= dm_ci.w) return;\n"
        "    device uint* dm_p = dm_capture + ((ulong(dm_iid - dm_ci.z) * dm_ci.y + dm_vid - dm_ci.x) * "
        "dm_gs.stride);\n";
    std::vector<std::string> words(kWords);
    for (int k = 0; k < kWords; ++k) words[k] = k == 3 || k == 4 || (k >= 19 && k % 4 == 3) ? "0x3f800000u" : "0u";
    for (const auto& [k, e] : stores) {
        if (k >= 0 && k < kWords) words[k] = absl::StrCat("as_type<uint>(", e, ")");
    }
    std::vector<std::string> ws;
    ws.reserve(kWords);
    for (int k = 0; k < kWords; ++k) ws.push_back(absl::StrFormat("dm_cw(dm_gs, dm_p, %d, %s);", k, words[k]));
    const std::string epilogue = absl::StrCat("{ ", py::Join(ws, " "), " return; }");
    if (!py::Contains(body, "return out;")) throw py::ValueError("geometry capture: missing return");
    return absl::StrFormat(
               "\nstruct dm_geometry_capture { device uint* dst; uint4 info; uint stride; short slot[%d]; };\n"
               "static inline void dm_cw(constant dm_geometry_capture& g, device uint* p, uint k, uint v) { short s = "
               "g.slot[k]; if (s >= 0) p[s] = v; }\n",
               kWords) +
           "vertex void main0_gs(" + params + ")\n{\n" + pro + py::Replace(body, "return out;", epilogue);
}

// ---- dmsh_build.py spv_single_lane

bool spirv::SingleLane(std::string& spv) {
    static const std::map<std::uint32_t, std::string> sg_ops = {
        {333, "true"},   {334, "pred"},  {335, "pred"},  {336, "true"},  {337, "value"}, {338, "value"},
        {339, "ballot"}, {345, "value"}, {346, "value"}, {347, "value"}, {348, "value"}};
    static const std::map<std::uint32_t, std::string> sg_builtins = {
        {36, "one"}, {41, "zero"}, {4416, "one4"}, {4417, "one4"}, {4418, "zero4"}, {4419, "one4"}, {4420, "zero4"}};
    using Ins = std::vector<std::uint32_t>;
    std::vector<std::uint32_t> words = Words(spv);
    if (words.size() < 5) return false;
    std::vector<std::uint32_t> hdr(words.begin(), words.begin() + 5);
    std::vector<Ins> ins;
    for (std::size_t i = 5; i < words.size();) {
        const std::uint32_t n = words[i] >> 16;
        if (n == 0) throw py::Fatal("[RED] a SPIR-V instruction of no words");
        ins.emplace_back(words.begin() + static_cast<std::ptrdiff_t>(i),
                         words.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(i + n, words.size())));
        i += n;
    }
    auto op = [](const Ins& x) { return x[0] & 0xffff; };
    auto is_builtin = [&](const Ins& x) {
        return op(x) == 71 && x.size() > 3 && x[2] == 11 && sg_builtins.count(x[3]);
    };
    bool any = false;
    for (const Ins& x : ins) any = any || sg_ops.count(op(x)) || is_builtin(x);
    if (!any) return false;
    for (const Ins& x : ins)
        if (op(x) >= 340 && op(x) <= 366 && !sg_ops.count(op(x))) return false;
    std::map<std::uint32_t, std::string> bvar;
    for (const Ins& x : ins)
        if (is_builtin(x)) bvar[x[1]] = sg_builtins.at(x[3]);
    std::map<std::uint32_t, std::pair<std::string, std::uint32_t>> bptr;
    for (const Ins& x : ins) {
        if ((op(x) == 65 || op(x) == 66) && x.size() > 3 && bvar.count(x[3])) {
            if (x.size() != 5 || !py::EndsWith(bvar[x[3]], "4")) return false;
            bptr[x[2]] = {bvar[x[3]], x[4]};
        }
    }
    for (const Ins& x : ins)
        if ((op(x) == 65 || op(x) == 66) && x.size() > 3 && bptr.count(x[3])) return false;
    std::optional<std::uint32_t> uint_t, bool_t, uvec4;
    for (const Ins& x : ins)
        if (!uint_t && op(x) == 21 && x.size() >= 4 && x[2] == 32 && x[3] == 0) uint_t = x[1];
    for (const Ins& x : ins)
        if (!bool_t && op(x) == 20) bool_t = x[1];
    for (const Ins& x : ins)
        if (!uvec4 && op(x) == 23 && x.size() >= 4 && uint_t && x[2] == *uint_t && x[3] == 4) uvec4 = x[1];
    if (!uint_t || !bool_t || !uvec4) return false;
    std::uint32_t bound = hdr[3];
    std::map<std::string, std::uint32_t> c;
    for (const char* nm : {"zero", "one", "zero4", "one4", "true"}) c[nm] = bound++;
    const std::vector<Ins> consts = {{43 | 4 << 16, *uint_t, c["zero"], 0},
                                     {43 | 4 << 16, *uint_t, c["one"], 1},
                                     {44 | 7 << 16, *uvec4, c["zero4"], c["zero"], c["zero"], c["zero"], c["zero"]},
                                     {44 | 7 << 16, *uvec4, c["one4"], c["one"], c["zero"], c["zero"], c["zero"]},
                                     {41 | 3 << 16, *bool_t, c["true"]}};
    auto ep_drop = [&](const Ins& x) {
        std::size_t k = 3;
        while (k < x.size()) {
            ++k;
            bool nul = false;
            for (int b = 0; b < 4; ++b) nul = nul || ((x[k - 1] >> (8 * b)) & 0xff) == 0;
            if (nul) break;
        }
        Ins y(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(std::min(k, x.size())));
        for (std::size_t i = k; i < x.size(); ++i)
            if (!bvar.count(x[i])) y.push_back(x[i]);
        y[0] = static_cast<std::uint32_t>(y.size()) << 16 | (x[0] & 0xffff);
        return y;
    };
    static const std::set<std::uint32_t> caps = {61, 62, 63, 64, 65, 66, 67, 68, 4423, 4431};
    std::vector<Ins> out;
    bool placed = false;
    for (Ins x : ins) {
        const std::uint32_t o = op(x);
        if (o == 17 && x.size() > 1 && caps.count(x[1])) continue;
        if (o == 10) {
            const std::string text(reinterpret_cast<const char*>(x.data() + 1), (x.size() - 1) * 4);
            if (py::Contains(text, "ballot") || py::Contains(text, "subgroup")) continue;
        }
        if ((o == 71 || o == 5) && x.size() > 1 && bvar.count(x[1])) continue;
        if (o == 59 && x.size() > 2 && bvar.count(x[2])) continue;
        if (o == 15) x = ep_drop(x);
        if (o == 54 && !placed) {
            out.insert(out.end(), consts.begin(), consts.end());
            placed = true;
        }
        if (o == 61 && x.size() > 3 && bvar.count(x[3])) {
            out.push_back({83 | 4 << 16, x[1], x[2], c[bvar[x[3]]]});
            continue;
        }
        if ((o == 65 || o == 66) && x.size() > 2 && bptr.count(x[2])) continue;
        if (o == 61 && x.size() > 3 && bptr.count(x[3])) {
            const auto& [nm, index] = bptr[x[3]];
            out.push_back({77 | 5 << 16, x[1], x[2], c[nm], index});
            continue;
        }
        if (auto k = sg_ops.find(o); k != sg_ops.end()) {
            if (k->second == "ballot") {
                out.push_back({169 | 6 << 16, x[1], x[2], x.at(4), c["one4"], c["zero4"]});
                continue;
            }
            out.push_back({83 | 4 << 16, x[1], x[2], k->second == "true" ? c["true"] : x.at(4)});
            continue;
        }
        out.push_back(x);
    }
    if (!placed) return false;
    hdr[3] = bound;
    std::vector<std::uint32_t> flat = hdr;
    for (const Ins& x : out) flat.insert(flat.end(), x.begin(), x.end());
    spv.assign(reinterpret_cast<const char*>(flat.data()), flat.size() * 4);
    return true;
}

}  // namespace nxmac::dmsh
