// nxmac::dmsh's ports of nxnative's shader build modules held to the Python functions: every call of them nxnative's
// unit tests made (recorded by fixtures/src/record_calls.py, made plain JSON by fixtures/src/calls_from_recording.py),
// with its arguments and what the Python returned or raised. Each port is called with the same arguments and must give
// the same value, in the same order where Python's has one (a dict's keys, a list's elements).
#include <zlib.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "json.h"
#include "modules.h"

namespace nxmac::dmsh {
namespace {

std::string ReadGz(const std::string& path) {
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[1 << 16];
    int n;
    while ((n = gzread(f, buf, sizeof buf)) > 0) out.append(buf, static_cast<std::size_t>(n));
    gzclose(f);
    return out;
}

Json OptStr(const std::optional<std::string>& s) { return s ? Json(*s) : Json(); }
Json OptInt(const std::optional<py::Int>& v) { return v ? Json(*v) : Json(); }
Json Pair(Json a, Json b) {
    Json out = Json::MakeArray();
    out.push_back(std::move(a));
    out.push_back(std::move(b));
    return out;
}
template <typename T>
Json List(const T& xs) {
    Json out = Json::MakeArray();
    for (const auto& x : xs) out.push_back(Json(x));
    return out;
}
std::vector<std::string> Strings(const Json& j) {
    std::vector<std::string> out;
    for (const Json& x : j.array()) out.push_back(x.as_string());
    return out;
}

side::Entry ToEntry(const Json& d) {
    side::Entry e;
    e.attr = d.at("attr").as_string();
    e.ret = d.at("ret").as_string();
    e.name = d.at("name").as_string();
    e.params = Strings(d.at("params"));
    e.decl = d.at("decl").as_string();
    e.args = d.at("args").as_string();
    e.start = d.at("start").as_int();
    e.head = d.at("head").as_int();
    e.end = d.at("end").as_int();
    return e;
}

Json FromEntry(const side::Entry& e) {
    Json d = Json::MakeObject();
    d.set("attr", e.attr);
    d.set("ret", e.ret);
    d.set("name", e.name);
    d.set("params", List(e.params));
    d.set("decl", e.decl);
    d.set("args", e.args);
    d.set("start", e.start);
    d.set("head", e.head);
    d.set("end", e.end);
    return d;
}

std::map<std::string, int> Locn(const Json& j) {
    std::map<std::string, int> out;
    for (const auto& [k, v] : j.object()) out[k] = static_cast<int>(v.as_int());
    return out;
}

Json UboPartJson(const reflect::UboPart& p) {
    if (p.kind.empty()) return Pair(p.a, p.b);
    return Pair(p.kind, p.b);
}

// The port's value for one recorded call
Json Call(const std::string& f, const Json::Array& a, const Json* kwargs) {
    auto s = [&](std::size_t i) { return a.at(i).as_string(); };
    auto n = [&](std::size_t i) { return a.at(i).as_int(); };
    if (f == "dmsh_alpha._colour0") {
        const auto [ty, nm] = alpha::Colour0(s(0), s(1));
        return ty.empty() ? Pair(Json(), Json()) : Pair(ty, nm);
    }
    if (f == "dmsh_alpha.entry") return OptStr(alpha::EntryText(s(0), ToEntry(a.at(1))));
    if (f == "dmsh_alpha.refused") return OptStr(alpha::Refused(s(0)));
    if (f == "dmsh_logic.refused") return OptStr(logic::Refused(s(0)));
    if (f == "dmsh_logic.constants") {
        std::set<int> t;
        for (const Json& x : a.at(0).array()) t.insert(static_cast<int>(x.as_int()));
        return logic::Constants(t);
    }
    if (f == "dmsh_logic.entry") {
        auto r = logic::EntryText(s(0), ToEntry(a.at(1)));
        if (!r) return Json();
        return Pair(r->first, List(r->second));
    }
    if (f == "dmsh_entry.drop_point_size") return entry::DropPointSize(s(0));
    if (f == "dmsh_entry.fs_uint") {
        auto [src, mask] = entry::FsUint(s(0));
        return Pair(OptStr(src), mask);
    }
    if (f == "dmsh_entry.int_outputs") {
        std::vector<std::string> cols;
        for (const auto& [k, v] : a.at(2).object()) cols.push_back(k);
        return List(entry::IntOutputs(s(0), s(1), cols));
    }
    if (f == "dmsh_fetch._entry_screen") return List(fetch::EntryScreen(s(0), Locn(a.at(1))));
    if (f == "dmsh_fetch._frag_pos") return fetch::FragPos(s(0), s(1), s(2));
    if (f == "dmsh_fetch._once") return fetch::Once(s(0), s(1));
    if (f == "dmsh_fetch._proj") return OptStr(fetch::Proj(s(0), s(1), s(2), Locn(a.at(3))));
    if (f == "dmsh_fetch.fold") {
        const Json* om = kwargs ? kwargs->find("om_only_ok") : nullptr;
        const fetch::Fold r = fetch::MakeFold(s(0), a.at(1), a.size() > 2 ? a.at(2).truthy() : om && om->truthy());
        return Pair(OptStr(r.src), r.meta);
    }
    if (f == "dmsh_fetch.screen_varyings") return List(fetch::ScreenVaryings(s(0)));
    if (f == "dmsh_fetch.select_guards") return fetch::SelectGuards(s(0));
    if (f == "dmsh_geometry.capture") return geometry::Capture(s(0));
    if (f == "dmsh_geometry.output_words") {
        Json out = Json::MakeObject();
        for (const auto& [k, e] : geometry::OutputWords(s(0))) out.set(absl::StrCat(k), e);
        return out;
    }
    if (f == "dmsh_pull._drop_entries") {
        std::set<std::string> keep;
        for (const Json& x : a.at(1).array()) keep.insert(x.as_string());
        return pull::DropEntries(s(0), keep);
    }
    if (f == "dmsh_pull.variant") {
        auto [src, info] = pull::Variant(s(0));
        return Pair(OptStr(src), info);
    }
    if (f == "dmsh_vin.variant") {
        auto [src, info] = vin::Variant(s(0));
        return Pair(OptStr(src), info);
    }
    if (f == "dmsh_reflect.c2_array") {
        auto r = reflect::C2Array(s(0), s(1));
        if (!r) return Json();
        return Pair(r->first, OptInt(r->second));
    }
    if (f == "dmsh_reflect.decode_tex") {
        auto r = reflect::DecodeTex(s(0));
        if (!r) return Json();
        Json out = Json::MakeArray();
        out.push_back(OptInt(r->t));
        out.push_back(OptInt(r->s));
        out.push_back(r->type);
        return out;
    }
    if (f == "dmsh_reflect.decode_ubo_img") {
        auto r = reflect::DecodeUboImg(s(0));
        return r ? Pair(r->first, r->second) : Json();
    }
    if (f == "dmsh_reflect.decode_ubo_tex") {
        auto r = reflect::DecodeUboTex(s(0));
        return r ? Pair(UboPartJson(r->first), UboPartJson(r->second)) : Json();
    }
    if (f == "dmsh_reflect.drv_reads") return reflect::DrvReads(s(0));
    if (f == "dmsh_reflect.in_word") return reflect::InWord(s(0), n(1));
    if (f == "dmsh_reflect.reflect") return reflect::Reflect(s(0), s(1), s(2), a.at(3));
    if (f == "dmsh_reflect.unfillable") return reflect::Unfillable(a.at(0), s(1));
    if (f == "dmsh_rewrite.blend_zero") {
        auto [src, k] = rewrite::BlendZero(s(0));
        return Pair(src, k);
    }
    if (f == "dmsh_rewrite.block_end") return rewrite::BlockEnd(s(0), n(1));
    if (f == "dmsh_rewrite.border_emu") return rewrite::BorderEmu(s(0));
    if (f == "dmsh_rewrite.first_store") {
        auto [src, stores] = rewrite::FirstStores(s(0), a.at(1));
        Json list = Json::MakeArray();
        for (const auto& st : stores) {
            Json t = Json::MakeArray();
            t.push_back(st.name);
            t.push_back(st.word);
            t.push_back(st.value);
            list.push_back(t);
        }
        return Pair(src, list);
    }
    if (f == "dmsh_rewrite.frag_z_snap") {
        auto [src, k] = rewrite::FragZSnap(s(0));
        return Pair(src, k);
    }
    if (f == "dmsh_rewrite.gather_offsets") return rewrite::GatherOffsets(s(0));
    if (f == "dmsh_rewrite.gather_quad") {
        auto r = rewrite::GatherQuad(Strings(a.at(0)));
        return r ? Pair(r->first, r->second) : Json();
    }
    if (f == "dmsh_rewrite.mul_extended") return rewrite::MulExtended(s(0));
    if (f == "dmsh_rewrite.shared_store_msl") return rewrite::SharedStoreMsl(s(0));
    if (f == "dmsh_rewrite.split_params") return List(rewrite::SplitParams(s(0)));
    if (f == "dmsh_rewrite.tg_spill") {
        auto [src, total] = rewrite::TgSpill(s(0));
        return Pair(src, total);
    }
    if (f == "dmsh_side._close") return side::Close(s(0), n(1), s(2), s(3));
    if (f == "dmsh_side._params") return List(side::Params(s(0)));
    if (f == "dmsh_side.body") {
        auto r = side::Body(s(0));
        if (!r) return Json();
        Json out = Json::MakeArray();
        out.push_back(r->start);
        out.push_back(r->head);
        out.push_back(r->end);
        return out;
    }
    if (f == "dmsh_side.calling") return side::Calling(ToEntry(a.at(0)), s(1), s(2), s(3));
    if (f == "dmsh_side.entries") {
        Json out = Json::MakeArray();
        for (const side::Entry& e : side::Entries(s(0))) out.push_back(FromEntry(e));
        return out;
    }
    if (f == "dmsh_side.entry") {
        auto e = side::FindEntry(s(0), s(1));
        return e ? FromEntry(*e) : Json();
    }
    if (f == "dmsh_side.shared_body") return OptStr(side::SharedBody(s(0)));
    if (f == "dmsh_side.unit") {
        const side::Unit u = side::MakeUnit(s(0));
        Json info = Json::MakeObject();
        if (!u.src) {
            info.set("why", u.why);
        } else {
            info.set("alpha", List(u.alpha));
            info.set("logic", List(u.logic));
        }
        return Pair(OptStr(u.src), info);
    }
    if (f == "dmsh_wide.wide_loads") return wide::WideLoads(s(0));
    throw std::logic_error("no port of " + f);
}

// The first place two texts differ, with a little of each around it
std::string Diff(const std::string& a, const std::string& b) {
    std::size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    const std::size_t from = i > 80 ? i - 80 : 0;
    return absl::StrCat("at ", i, "\n  port:   ", a.substr(from, 240), "\n  python: ", b.substr(from, 240));
}

TEST(DmshCallsTest, EveryRecordedCallGivesWhatPythonGave) {
    const std::string text = ReadGz(NXMAC_DMSH_FIXTURES "/calls.jsonl.gz");
    ASSERT_FALSE(text.empty());
    std::map<std::string, int> calls, failed;
    std::size_t at = 0;
    int shown = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const auto rec = JsonParse(std::string_view(text).substr(at, end - at));
        at = end + 1;
        ASSERT_TRUE(rec.ok()) << rec.status();
        const std::string f = rec->at("f").as_string();
        ++calls[f];
        std::string got, want;
        if (const Json* e = rec->find("e")) {
            want = e->as_string();
        } else {
            want = JsonLine(rec->at("r"));
        }
        try {
            got = JsonLine(Call(f, rec->at("a").array(), rec->find("k")));
        } catch (const py::ValueError& e) {
            got = absl::StrCat("ValueError: ", e.what());
        }
        if (got != want) {
            ++failed[f];
            if (shown++ < 12) ADD_FAILURE() << f << " " << Diff(got, want);
        }
    }
    for (const auto& [f, k] : failed) ADD_FAILURE() << f << ": " << k << " of " << calls[f] << " calls differ";
    EXPECT_GT(calls.size(), 40u);
}

}  // namespace
}  // namespace nxmac::dmsh
