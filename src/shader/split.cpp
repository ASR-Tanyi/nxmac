#include "nxmac/shader/split.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>

#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/strip.h"
#include "files.h"
#include "nvn.h"
#include "nxmac/rewriter/module.h"
#include "readers.h"
#include "scan.h"

namespace nxmac::shader {
namespace {

namespace stdfs = std::filesystem;
using internal::Bytes;
using internal::Contains;
using internal::Found;
using internal::Hex;
using internal::Keyed;
using internal::Shader;
using internal::VarPair;

constexpr const char* kSingleStage[] = {"#GLSLC@", "#VIPER", "#UE",      "#MTSPK@",    "#MGFX@",  "#NLG",   "#PGSO",
                                        "#RAW@",   "#TFXO@", "#NEEDLE@", "#SNOWDROP@", "#PHYRE@", "#SUMO@", "#NVNSH@"};

// The split's own stop: what red() printed, as a status
struct Stop {
    absl::Status status;
};

std::string Group(const ShaderProfile& p, const std::string& label) {
    if (absl::StartsWith(label, "main")) return "main";
    for (const auto& [part, group] : p.groups) {  // the first match wins
        if (absl::StrContains(label, part)) return group;
    }
    return "other";
}

// os.path.abspath, then dirname / basename as Python takes them (no trailing separator)
stdfs::path Absolute(const std::string& path) {
    std::error_code ec;
    stdfs::path p = stdfs::absolute(path, ec).lexically_normal();
    if (p.has_filename() || p == p.root_path()) return p;
    return p.parent_path();  // "x/" -> "x"
}

std::string TableHeader(std::string_view text) { return absl::StrCat(text, "\n"); }

// Python's f'{n:,}'
std::string Thousands(std::size_t n) {
    std::string digits = absl::StrCat(n), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

class Splitter {
public:
    explicit Splitter(const SplitOptions& o) : o_(o), p_(*o.profile), skip_{p_.key_skip_offset, p_.key_skip_length} {}

    absl::StatusOr<SplitResult> Run();

private:
    void Print(const std::string& line) {
        const std::lock_guard<std::mutex> lock(print_mu_);
        if (o_.print) o_.print(line);
    }
    absl::Status ScanMain(const SplitVersion& v, const std::string& tag);
    void ScanFiles();
    absl::StatusOr<SplitResult> Merge();

    const SplitOptions& o_;
    const ShaderProfile& p_;
    internal::KeySkip skip_;
    std::mutex print_mu_;

    struct Job {
        const internal::RomFsSource* src;
        const internal::RomFsSource::File* file;
        std::string rel;
    };
    std::vector<std::unique_ptr<internal::RomFsSource>> sources_;
    std::vector<Job> jobs_;
    // What the scans found, taken in shader_split.py's order (each version's main, then every file) as soon as the
    // results before it are in: every key once, the first found, and the source groups in the order they first came
    void Absorb(internal::FileResult&& r);
    std::vector<std::string> bad_;
    std::vector<std::string> order_;
    std::unordered_map<std::string, Shader> keys_;
    std::vector<std::pair<std::string, std::size_t>> per_src_;
    std::set<VarPair> all_pairs_;
    std::optional<absl::Status> conflict_;  // the first key whose bytes differ: the split stops once all is read
};

void Splitter::Absorb(internal::FileResult&& r) {
    if (r.unreadable) bad_.push_back(*r.unreadable);
    if (conflict_) return;
    all_pairs_.insert(r.pairs.begin(), r.pairs.end());
    for (auto& [key, s] : r.uniq) {
        const auto it = keys_.find(key);
        if (it != keys_.end()) {
            if (!internal::SameShader(it->second.ctl, it->second.code, s.ctl, s.code, skip_)) {
                conflict_ = absl::FailedPreconditionError(
                    absl::StrCat("key ", key, " is the same but the control section or code area differs (", s.where,
                                 " / ", it->second.where, ")"));
                return;
            }
            continue;
        }
        const std::string g = Group(p_, s.where);
        auto src = std::find_if(per_src_.begin(), per_src_.end(), [&](const auto& e) { return e.first == g; });
        if (src == per_src_.end()) {
            per_src_.emplace_back(g, 1);
        } else {
            ++src->second;
        }
        order_.push_back(key);
        keys_.emplace(key, std::move(s));
    }
}

absl::Status ScanError(const std::exception& e) {
    if (const auto* np = dynamic_cast<const internal::NotPorted*>(&e)) return absl::UnimplementedError(np->what());
    if (const auto* red = dynamic_cast<const internal::Red*>(&e)) return absl::FailedPreconditionError(red->what());
    if (const auto* py = dynamic_cast<const internal::PyError*>(&e)) {
        return absl::DataLossError(absl::StrCat(py->type(), ": ", py->what()));
    }
    return absl::InternalError(e.what());
}

absl::Status Splitter::ScanMain(const SplitVersion& v, const std::string& tag) {
    absl::StatusOr<rewriter::Module> module = rewriter::ReadModule(v.main);
    if (!module.ok()) return absl::FailedPreconditionError(module.status().message());  // red(str(nso.Error))
    const Bytes img(reinterpret_cast<const char*>(module->image.data()), module->image.size());
    const std::string label = tag.empty() ? "main" : absl::StrCat("main@", tag);
    internal::Context cx{skip_, {}, [this](const std::string& line) { Print(line); }};
    Found found;
    try {
        internal::ScanBlob(cx, img, label, found);
        for (Shader& s : internal::GlslcShaders(img, label, false)) found.push_back(std::move(s));
        for (Shader& s : internal::TfxoStreams(img, label)) found.push_back(std::move(s));  // YEBIS's effect file
        internal::RetroMainForms(img, label);  // Retro's startup material archive
        if (Contains(img, "rgxd")) {
            for (Shader& s : internal::RgxdShaders(cx, img, label)) found.push_back(std::move(s));
        }
        std::set<std::string, std::less<>> known;  // compared at their own length, as raw_shaders reads them
        for (const Shader& s : found) known.emplace(internal::Head(s.ctl, internal::U32(s.ctl, 0x20)));
        for (Shader& s : internal::RawShaders(cx, img, label, known)) found.push_back(std::move(s));
        // GLSLC outputs the game compiles at run time, captured on the Mac
        const stdfs::path cap = o_.glslc_capture.empty() ? Absolute(v.romfs).parent_path() / "glslc_capture"
                                                         : stdfs::path(o_.glslc_capture);
        std::vector<std::string> names;
        std::error_code ec;
        if (stdfs::is_directory(cap, ec)) {
            for (const stdfs::directory_entry& e : stdfs::directory_iterator(cap, ec)) {
                const std::string n = e.path().filename().string();
                if (absl::EndsWith(n, ".glslc") && !absl::StartsWith(n, "._")) names.push_back(n);
            }
            std::sort(names.begin(), names.end());
        }
        if (names.empty() && internal::GlslSources(img)) {
            bool imports = false;
            for (const auto& [name, defined] : internal::DynSyms(img)) imports |= !defined && name == "glslcCompile";
            std::string module_name;
            if (imports) {  // a module of the profile's that defines glslcCompile, beside main
                const stdfs::path exefs = Absolute(v.main).parent_path();
                for (const std::string& m : p_.modules.empty() ? std::vector<std::string>{"main"} : p_.modules) {
                    if (m == "main" || !stdfs::exists(exefs / m, ec)) continue;
                    absl::StatusOr<rewriter::Module> other = rewriter::ReadModule((exefs / m).string());
                    if (!other.ok()) return absl::FailedPreconditionError(other.status().message());
                    const Bytes oimg(reinterpret_cast<const char*>(other->image.data()), other->image.size());
                    bool defines = false;
                    for (const auto& [name, defined] : internal::DynSyms(oimg))
                        defines |= defined && name == "glslcCompile";
                    if (defines) {
                        module_name = m;
                        break;
                    }
                }
            }
            if (!module_name.empty()) {
                Print(absl::StrCat(
                    "[RED] main holds GLSL sources and imports glslcCompile, but there are no captured GLSLC "
                    "outputs in ",
                    cap.string(),
                    ": the shaders the game compiles at run time will have no "
                    "translation (the NSP script checks an empty capture against the offline split)"));
            }
        }
        for (const std::string& n : names) {
            std::ifstream in(cap / n, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            for (Shader& s : internal::GlslcShaders(bytes, absl::StrCat("/glslc_capture/", n), true))
                found.push_back(std::move(s));
        }
        // Unreal Engine shader code archives decoded on the Mac: not ported
        const stdfs::path ued = Absolute(v.romfs).parent_path() / "ue_shaders";
        if (stdfs::is_directory(ued, ec)) {
            for (const stdfs::directory_entry& e : stdfs::directory_iterator(ued, ec)) {
                const std::string n = e.path().filename().string();
                if (absl::EndsWith(n, ".ushaderbytecode") && !absl::StartsWith(n, "._")) {
                    return absl::UnimplementedError(absl::StrCat(
                        ued.string(), ": Unreal Engine shader code archives: their reader is not ported to nxmac yet"));
                }
            }
        }
    } catch (const std::exception& e) {
        return ScanError(e);
    }
    // {skey: shader}: a key found twice keeps its first place and its last bytes
    internal::FileResult r;
    r.rel = "main";
    std::unordered_map<std::string, std::size_t> at;
    for (const Shader& s : found) {
        std::string key = internal::Skey(s.ctl, s.code, skip_);
        const auto it = at.find(key);
        if (it != at.end()) {
            r.uniq[it->second].second = s;
        } else {
            at.emplace(key, r.uniq.size());
            r.uniq.emplace_back(std::move(key), s);
        }
    }
    r.pairs = internal::VarPairs(cx, found);
    Absorb(std::move(r));
    return absl::OkStatus();
}

void Splitter::ScanFiles() {
    const std::size_t total = jobs_.size();
    std::vector<std::optional<internal::FileResult>> out(total);
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<std::size_t> next{0};
    std::size_t done = 0;  // under mu
    const auto worker = [&]() {
        for (std::size_t k = next++; k < total; k = next++) {
            const Job& j = jobs_[k];
            internal::FileResult r =
                internal::ScanFile(*j.src, *j.file, j.rel, skip_, [this](const std::string& line) { Print(line); });
            {
                const std::lock_guard<std::mutex> lock(mu);
                out[k] = std::move(r);
                ++done;
            }
            cv.notify_one();
        }
    };
    const int n = std::max(1, std::min<int>(o_.jobs, static_cast<int>(std::max<std::size_t>(total, 1))));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t) threads.emplace_back(worker);
    // the results taken in order as they come; progress: <done>/<total> at most twice a second, and when all are done
    // (the NSP script shows it)
    std::size_t taken = 0;
    auto shown = std::chrono::steady_clock::now();
    while (taken < total) {
        std::vector<internal::FileResult> ready;
        std::size_t finished = 0;
        {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return out[taken].has_value(); });
            while (taken + ready.size() < total) {
                std::optional<internal::FileResult>& slot = out[taken + ready.size()];
                if (!slot.has_value()) break;
                ready.push_back(std::move(slot.value()));
                slot.reset();
            }
            finished = done;
        }
        taken += ready.size();
        for (internal::FileResult& r : ready) Absorb(std::move(r));
        const auto now = std::chrono::steady_clock::now();
        if (finished == total || now - shown >= std::chrono::milliseconds(500)) {
            if (finished < total || taken == total) Print(absl::StrCat("progress: ", finished, "/", total));
            shown = now;
        }
    }
    for (std::thread& t : threads) t.join();
}

absl::StatusOr<SplitResult> Splitter::Run() {
    if (!o_.profile) return absl::InvalidArgumentError("no game profile");
    if (o_.versions.empty()) return absl::InvalidArgumentError("no RomFS and main to split");
    // the files of every version (a later one's names prefixed with its folder's name), then of every DLC
    for (std::size_t i = 0; i < o_.versions.size(); ++i) {
        const SplitVersion& v = o_.versions[i];
        absl::StatusOr<std::unique_ptr<internal::RomFsSource>> src = internal::RomFsSource::Open(v.romfs);
        if (!src.ok()) return src.status();
        const std::string tag = i ? Absolute(v.romfs).parent_path().filename().string() : "";
        for (const internal::RomFsSource::File& f : (*src)->files()) {
            jobs_.push_back({src->get(), &f, tag.empty() ? f.rel : absl::StrCat(tag, "/", f.rel)});
        }
        sources_.push_back(*std::move(src));
        if (absl::Status s = ScanMain(v, tag); !s.ok()) return s;
    }
    for (const std::string& dlc : o_.dlc) {
        absl::StatusOr<std::unique_ptr<internal::RomFsSource>> src = internal::RomFsSource::Open(dlc);
        if (!src.ok()) return src.status();
        // os.path.basename(os.path.normpath(dlc))
        stdfs::path norm = stdfs::path(dlc).lexically_normal();
        if (!norm.has_filename() && norm.has_parent_path() && norm != norm.root_path()) norm = norm.parent_path();
        std::string name = norm.filename().string();
        if ((*src)->image()) name = std::string(absl::StripSuffix(name, ".img"));
        for (const internal::RomFsSource::File& f : (*src)->files()) {
            jobs_.push_back({src->get(), &f, absl::StrCat("aoc/", name, "/", f.rel)});
        }
        sources_.push_back(*std::move(src));
    }
    // What shader_split.py takes apart as a whole before the scan: not ported
    for (const auto& src : sources_) {
        if (src->Exists("Pack/ZsDic.pack.zs")) {
            return absl::UnimplementedError(absl::StrCat(
                src->root(),
                "/Pack/ZsDic.pack.zs: zstd dictionaries (Tears of the Kingdom): zstd is not ported to nxmac yet"));
        }
    }
    for (const Job& j : jobs_) {
        if (std::optional<std::string> what = internal::UnportedArchive(*j.src, *j.file)) {
            return absl::UnimplementedError(
                absl::StrCat("/", j.rel, ": ", *what, ": its reader is not ported to nxmac yet"));
        }
    }
    ScanFiles();
    return Merge();
}

absl::StatusOr<SplitResult> Splitter::Merge() {
    SplitResult out;
    out.unreadable = bad_.size();
    if (!bad_.empty()) {
        Print(absl::StrCat("[RED] ", bad_.size(),
                           " file(s) could not be read, their shaders are left out (first: ", bad_[0], ")"));
    }
    if (conflict_) return *conflict_;
    std::vector<std::string>& order = order_;
    std::unordered_map<std::string, Shader>& keys = keys_;
    std::vector<std::pair<std::string, std::size_t>>& per_src = per_src_;
    const std::set<VarPair>& all_pairs = all_pairs_;
    // pairs of BAHS program variations that BNSH variations do not give
    std::set<std::pair<std::string, std::string>> pairs, prog;
    for (const VarPair& v : all_pairs) {
        if (!v.prog) pairs.emplace(v.a, v.b);
    }
    for (const VarPair& v : all_pairs) {
        if (v.prog && !pairs.count({v.a, v.b})) prog.emplace(v.a, v.b);
    }
    // single-stage programs: each vertex shader with a cover of the pixel shaders' inputs
    std::vector<std::string> single;
    for (const std::string& k : order) {
        const Shader& s = keys.at(k);
        if (s.st != "vs" || absl::StrContains(s.where, "#var")) continue;
        if (std::any_of(std::begin(kSingleStage), std::end(kSingleStage),
                        [&](const char* m) { return absl::StrContains(s.where, m); })) {
            single.push_back(k);
        }
    }
    if (!single.empty()) {
        std::map<std::string, std::set<int>> ins;
        try {
            for (const std::string& k : order) {
                const Shader& s = keys.at(k);
                if (s.st != "ps") continue;
                const std::vector<int> in = internal::PsInputs(s.code);
                ins[k] = std::set<int>(in.begin(), in.end());
            }
        } catch (const std::exception& e) {
            return ScanError(e);
        }
        std::set<int> left;
        for (const auto& [k, in] : ins) left.insert(in.begin(), in.end());
        std::vector<std::string> cover;
        while (!left.empty()) {  // the pixel shaders' inputs are left, so there is one
            auto best = ins.end();
            std::size_t best_n = 0;
            for (auto it = ins.begin(); it != ins.end(); ++it) {  // by key: ties go to the smaller one
                std::size_t n = 0;
                for (const int x : it->second) n += left.count(x);
                if (best == ins.end() || n > best_n) {
                    best = it;
                    best_n = n;
                }
            }
            cover.push_back(best->first);
            for (const int x : best->second) left.erase(x);
        }
        for (const std::string& v : single) {
            for (const std::string& c : cover) pairs.emplace(v, c);
        }
        if (!cover.empty()) {
            Print(absl::StrCat(
                "single-stage programs: ", Thousands(std::set<std::string>(single.begin(), single.end()).size()),
                " vertex shaders are paired with a cover of ", cover.size(), " pixel shader(s) (",
                absl::StrJoin(cover, " "), ")"));
        }
    }
    for (auto it = prog.begin(); it != prog.end();) it = pairs.count(*it) ? prog.erase(it) : std::next(it);

    // the package and the map: by source group, then where, then key (the same input gives the same output)
    std::vector<std::string> sorted = order;
    std::unordered_map<std::string, std::string> groups;
    for (const std::string& k : order) groups.emplace(k, Group(p_, keys.at(k).where));
    std::sort(sorted.begin(), sorted.end(), [&](const std::string& a, const std::string& b) {
        const std::string& ga = groups.at(a);
        const std::string& gb = groups.at(b);
        if (ga != gb) return ga < gb;
        const std::string& wa = keys.at(a).where;
        const std::string& wb = keys.at(b).where;
        if (wa != wb) return wa < wb;
        return a < b;
    });
    std::map<std::string, std::size_t> count;  // records per package file
    std::map<std::string, std::size_t> stat;
    std::map<std::string, std::map<std::string, std::string>> facts;  // key -> fact name -> JSON text
    std::string rows;
    std::vector<std::string> wheres;
    for (const std::string& key : sorted) {
        const Shader& s = keys.at(key);
        const std::string name = absl::StrCat(s.st, "_", groups.at(key), ".bin");
        std::string id;
        if (!absl::HexStringToBytes(key, &id))
            return absl::InternalError(absl::StrCat("key ", key, " is not hex digits"));
        std::string rec = absl::StrCat("BTCDBTCD", id, std::string(0x28, '\0'));
        for (int i = 0; i < 4; ++i) {
            rec[0x28 + i] = static_cast<char>(s.ctl.size() >> (8 * i));
            rec[0x2c + i] = static_cast<char>(s.code.size() >> (8 * i));
        }
        rec += s.code;
        rec += s.ctl;
        if (internal::Find(rec, internal::kCtlMagic, 0x38) != static_cast<std::int64_t>(0x38 + s.code.size())) {
            return absl::FailedPreconditionError(absl::StrCat(
                "key ", key,
                ": the control magic appears in the code area, dmsh_build would split the record wrongly"));
        }
        if (internal::Count(rec, "BTCDBTCD") != 1) {
            return absl::FailedPreconditionError(
                absl::StrCat("key ", key, ": the record separator appears inside the record"));
        }
        absl::StrAppend(&rows, name, "\t", count[name]++, "\t", internal::StageName(s.st), "\t", key, "\t", s.where,
                        "\n");
        wheres.push_back(s.where);
        out.package[name] += rec;
        ++stat[s.st];
        if (s.st == "cs") {
            // NVN2: the thread group at +0x58, shared memory at +0x64
            const bool nvn2 = internal::Nvn2Ctl(s.ctl);
            const std::int64_t tg = nvn2 ? 0x58 : 0x748, sh = nvn2 ? 0x64 : 0x754;
            std::uint32_t x = 0, y = 0, z = 0, shared = 0;
            try {
                x = internal::U32(s.ctl, tg);
                y = internal::U32(s.ctl, tg + 4);
                z = internal::U32(s.ctl, tg + 8);
                shared = internal::U32(s.ctl, sh);
            } catch (const std::exception& e) {
                return ScanError(e);
            }
            if (static_cast<std::uint64_t>(x) * y * z == 0) {
                return absl::FailedPreconditionError(
                    absl::StrCat("compute shader ", key, ": control +", Hex(tg), " thread group is 0 (", s.where, ")"));
            }
            facts[key]["local"] = absl::StrCat("[", x, ",", y, ",", z, "]");
            facts[key]["shared"] = absl::StrCat((static_cast<std::uint64_t>(shared) + 15) / 16 * 16);
        }
    }
    const auto any = [&](auto&& f) { return std::any_of(wheres.begin(), wheres.end(), f); };
    const auto has = [&](const char* m) { return any([&](const std::string& w) { return absl::StrContains(w, m); }); };
    std::vector<std::string> kinds;
    const auto kind = [&](const char* name, bool hit) {
        if (hit) kinds.emplace_back(name);
    };
    kind("BNSH / BAHS",
         any([](const std::string& w) { return absl::StrContains(w, "#BNSH@") || absl::StrContains(w, "#BAHS@"); }));
    kind("Unity Shader", any([](const std::string& w) {
             return absl::StrContains(w, "#var") && !absl::StrContains(w, "#PMAP@") &&
                    !absl::StrContains(w, "#BNSH@") && !absl::StrContains(w, "#MSHD#") &&
                    !absl::StrContains(w, "#SDF#") && !absl::StrContains(w, "#GLSLC@") &&
                    !absl::StrContains(w, "#SNVN@");
         }));
    kind("Retro Studios SNVN", has("#SNVN@"));
    kind("phx PMAP", has("#PMAP@"));
    kind("MSHD", has("#MSHD#"));
    kind("GLSLC", has("#GLSLC@"));
    kind("RE Engine SDF", has("#SDF#"));
    kind("MT Framework SPK", has("#MTSPK@"));
    kind("MonoGame MGFX", has("#MGFX@"));
    kind("PlatinumGames shader objects", has("#PGSO"));
    kind("Snowdrop shader objects", has("#SNOWDROP@"));
    kind("Unreal Engine shader code archives", has("#UE"));
    out.shader_map =
        absl::StrCat("# kind: evidence  ", p_.note, " (", absl::StrJoin(kinds, " / "),
                     " in RomFS + main) \xe2\x86\x92 control section + code area keys (dm/gfx/tools/shader_split.py)\n",
                     "# column 5 records where each key first appears; no PC shader pairing supplied\n",
                     "package\tindex\tstage\tdm_ctl_sha16\tsource\n", rows);
    // the profile's hand-written facts, merged over the generated ones
    for (const auto& [key, members] : p_.facts) {
        for (const auto& [name, value] : members) facts[key][name] = value;
    }
    out.shader_facts = TableHeader(
        "# Answer table for translation queries (generated by dm/gfx/tools/shader_split.py, do not edit): key <TAB> "
        "fact JSON. "
        "No PC shader pairing supplied: only the thread group / shared memory of compute shaders (taken from the "
        "control "
        "section); other queries get the translator defaults");
    for (const auto& [key, members] : facts) {
        std::vector<std::string> parts;
        parts.reserve(members.size());
        for (const auto& [name, value] : members) parts.push_back(absl::StrCat("\"", name, "\":", value));
        absl::StrAppend(&out.shader_facts, key, "\t{", absl::StrJoin(parts, ","), "}\n");
    }
    out.shader_pairs = TableHeader(
        "# kind: evidence  vertex / pixel shader pairs within BNSH variations (generated by "
        "dm/gfx/tools/shader_split.py, do "
        "not edit): vertex key <TAB> pixel key");
    for (const auto& [a, b] : pairs) absl::StrAppend(&out.shader_pairs, a, "\t", b, "\n");
    out.pso_pairs = TableHeader(
        "# kind: evidence  supplemental vertex / pixel shader pairs (BAHS program variations and legacy Unity "
        "pass-input "
        "covers) for vertex output padding and the pipeline prebuild (generated by dm/gfx/tools/shader_split.py, do "
        "not "
        "edit): vertex key <TAB> pixel key");
    for (const auto& [a, b] : prog) absl::StrAppend(&out.pso_pairs, a, "\t", b, "\n");
    out.shaders = sorted.size();
    std::vector<std::string> by_stage, by_src;
    for (const auto& [st, n] : stat) by_stage.push_back(absl::StrCat(internal::StageName(st), " ", n));
    // most_common(): by count, ties in the order the groups first came
    std::stable_sort(per_src.begin(), per_src.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [g, n] : per_src) by_src.push_back(absl::StrCat(g, " ", n));
    Print(absl::StrCat("shaders: ", sorted.size(), " (", absl::StrJoin(by_stage, ", "), "); sources ",
                       absl::StrJoin(by_src, ", "), "; compute shader answers ", facts.size(),
                       " rows; variation pairs ", pairs.size()));
    return out;
}

absl::Status WriteFile(const stdfs::path& path, std::string_view bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !f.flush()) {
        return absl::InternalError(absl::StrFormat("%s: cannot write it", path.string()));
    }
    return absl::OkStatus();
}

}  // namespace

absl::StatusOr<SplitResult> Split(const SplitOptions& options) {
    Splitter s(options);
    return s.Run();
}

absl::Status WriteSplit(const SplitResult& result, const std::string& dir) {
    std::error_code ec;
    const stdfs::path pkg = stdfs::path(dir) / "pkg";
    stdfs::create_directories(pkg, ec);
    if (ec) return absl::InternalError(absl::StrFormat("%s: %s", pkg.string(), ec.message()));
    for (const stdfs::directory_entry& e : stdfs::directory_iterator(pkg, ec)) {
        if (e.path().extension() == ".bin") stdfs::remove(e.path(), ec);
    }
    for (const auto& [name, bytes] : result.package) {
        if (absl::Status s = WriteFile(pkg / name, bytes); !s.ok()) return s;
    }
    for (const auto& [name, text] : {std::pair<const char*, const std::string*>{"shader_map.tsv", &result.shader_map},
                                     {"shader_facts.tsv", &result.shader_facts},
                                     {"shader_pairs.tsv", &result.shader_pairs},
                                     {"pso_pairs.tsv", &result.pso_pairs}}) {
        if (absl::Status s = WriteFile(stdfs::path(dir) / name, *text); !s.ok()) return s;
    }
    return absl::OkStatus();
}

}  // namespace nxmac::shader
