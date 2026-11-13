// dmsh_build.py: the shader build's driver. Its steps are the Python's, in its order, with its messages; what differs
// is said where it does: nxmac's own cache (a folder of finished shaders, keyed by everything that goes into one, and
// of compiled libraries keyed by their Metal source), the library written from that cache, no iOS libraries.
#include "nxmac/dmsh/build.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <thread>

#include "abi.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "cross.h"
#include "json.h"
#include "metal.h"
#include "modules.h"
#include "nxmac/fs/crypto.h"
#include "nxmac/fs/romfs_image.h"
#include "nxmac/fs/storage.h"
#include "process.h"
#include "re.h"

namespace nxmac::dmsh {
namespace {

namespace stdfs = std::filesystem;
using py::Int;
using namespace std::string_view_literals;

constexpr std::string_view kXlateSource =
    "657232fd201f16a1";  // dmsh_chain.lock xlate_source: the translator's embedded source digest
constexpr std::string_view kXlateBinary =
    "be6148d9cd7b6022";  // dmsh_chain.lock dmsh_xlate: nxnative's committed translator
constexpr std::string_view kLockMetal = "Apple metal version 32023.922 (metalfe-32023.922.4)";
constexpr std::string_view kSpirvCross = "vulkan-sdk-1.4.363.0 f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7";
constexpr std::string_view kStamp = "inputs.tsv";
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const std::vector<std::string> kXc = {"--msl",
                                      "--msl-version",
                                      "20300",
                                      "--msl-argument-buffers",
                                      "--msl-texture-buffer-native",
                                      "--msl-pad-fragment-output"};
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const std::vector<std::string> kInputTables = {"shader_facts.tsv", "shader_map.tsv", "shader_map.tsv.conflicts.tsv",
                                               "shader_pairs.tsv", "pso_pairs.tsv",  "bf_want.tsv",
                                               "geo_mesh.tsv",     "dual_want.tsv"};
constexpr std::string_view kCtlMagic = "\x34\x12\x76\x98";

std::string Sha256(std::string_view data) {
    const fs::Sha256Hash h = fs::Sha256(std::span(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
    return std::string(reinterpret_cast<const char*>(h.data()), h.size());
}
std::string Sha256Hex(std::string_view data) { return absl::BytesToHexString(Sha256(data)); }

std::optional<std::string> ReadOpt(const stdfs::path& p) {
    absl::StatusOr<std::string> s = ReadFile(p.string());
    if (!s.ok()) return std::nullopt;
    return *s;
}

std::uint32_t U32(std::string_view b, std::size_t at) {
    if (at + 4 > b.size())
        throw py::Fatal(absl::StrFormat("[RED] unpack_from requires a buffer of at least %d bytes", at + 4));
    std::uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
std::uint64_t U64(std::string_view b, std::size_t at) {
    if (at + 8 > b.size())
        throw py::Fatal(absl::StrFormat("[RED] unpack_from requires a buffer of at least %d bytes", at + 8));
    std::uint64_t v;
    std::memcpy(&v, b.data() + at, 8);
    return v;
}
std::uint16_t U16(std::string_view b, std::size_t at) {
    if (at + 2 > b.size())
        throw py::Fatal(absl::StrFormat("[RED] unpack_from requires a buffer of at least %d bytes", at + 2));
    std::uint16_t v;
    std::memcpy(&v, b.data() + at, 2);
    return v;
}

// The running program's bytes, digested: every change to nxmac's code changes every shader's cache key (dmsh_build.py's
// CODE_FP digests its own sources the same way)
std::string SelfDigest() {
    std::string path;
#if defined(__APPLE__)
    char buf[4096];
    std::uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) path = buf;
#else
    path = "/proc/self/exe";
#endif
    absl::StatusOr<std::string> bytes = ReadFile(path);
    return bytes.ok() ? Sha256Hex(*bytes) : "unknown";
}

// One package file, mapped: its records by key (record_index)
class Package {
public:
    ~Package() {
        if (data_) munmap(data_, size_);
    }
    static absl::StatusOr<std::unique_ptr<Package>> Open(const std::string& path) {
        auto p = std::make_unique<Package>();
        const int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return absl::NotFoundError(absl::StrCat("[Errno 2] No such file or directory: '", path, "'"));
        struct stat st{};
        fstat(fd, &st);
        p->size_ = static_cast<std::size_t>(st.st_size);
        if (p->size_) {
            void* m = mmap(nullptr, p->size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m == MAP_FAILED) {
                close(fd);
                return absl::UnavailableError(absl::StrCat(path, ": cannot be mapped"));
            }
            p->data_ = m;
        }
        close(fd);
        const std::string_view all = p->view();
        std::vector<std::size_t> offs;
        for (std::size_t at = all.find("BTCDBTCD"); at != std::string_view::npos; at = all.find("BTCDBTCD", at + 8))
            offs.push_back(at);
        offs.push_back(all.size());
        for (std::size_t k = 0; k + 1 < offs.size(); ++k) {
            const std::string key = absl::BytesToHexString(
                all.substr(offs[k] + 8, std::min<std::size_t>(8, all.size() - std::min(all.size(), offs[k] + 8))));
            p->records_[key] = {offs[k], offs[k + 1]};
        }
        return p;
    }
    std::string_view view() const { return {static_cast<const char*>(data_), size_}; }
    const std::map<std::string, std::pair<std::size_t, std::size_t>>& records() const { return records_; }

private:
    void* data_ = nullptr;
    std::size_t size_ = 0;
    std::map<std::string, std::pair<std::size_t, std::size_t>> records_;
};

struct Result {
    std::string key, st, err;
    Json refl;  // null unless st is "ok"
};

struct Stats {
    std::mutex lock;
    std::map<std::string, int> vin, pull, side, bf, pre;
    void Add(std::map<std::string, int>& m, const std::string& k, int n = 1) {
        std::lock_guard<std::mutex> l(lock);
        m[k] += n;
    }
};

// The job's working state of one shader (finish's j)
struct Job {
    Json j;
    std::string c1;  // bytes
};

template <typename F>
void ParallelFor(std::size_t n, int jobs, const F& f) {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> threads;
    const int k = std::max(1, std::min<int>(jobs, static_cast<int>(n)));
    threads.reserve(static_cast<std::size_t>(k));
    for (int t = 0; t < k; ++t) {
        threads.emplace_back([&] {
            for (std::size_t i = next++; i < n; i = next++) f(i);
        });
    }
    for (std::thread& t : threads) t.join();
}

class Builder {
public:
    explicit Builder(const BuildOptions& o) : o_(o) {}
    absl::StatusOr<BuildResult> Run();

private:
    void Print(const std::string& line) {
        std::lock_guard<std::mutex> l(print_lock_);
        if (o_.print) o_.print(line);
    }
    absl::Status LoadProfile();
    absl::Status CheckTools();
    std::string RecordOf(const std::string& key);  // throws py::Fatal
    std::string ShaderKey(const std::string& key, const std::string& name, const std::string& rec);
    std::optional<std::string> EntryDir(const std::string& key) const;  // the cache entry of a finished shader
    std::optional<std::string> Output(const std::string& key, const std::string& name) const;  // output('mac', name)
    Job JobFor(const std::string& key, const std::string& name, const std::string& rec, const std::string& w);
    std::optional<Json> TfLayout(const std::string& key, std::string_view ctl);
    std::string Translate(const std::vector<std::string>& batch, int n);
    Result Fin(const std::string& key, const std::string& wb);
    Result Finish(const std::string& key, const std::string& w, Job& job, const std::string& out);
    Json PsLibraries(const std::string& key, const std::string& msl, const std::string& full, const Json& bindings,
                     const std::string& w, const std::string& out);
    void VinFinish(const std::string& key, const std::string& full, Json& refl, const std::string& w,
                   const std::string& out);
    void PullFinish(const std::string& key, const std::string& full, Json& refl, const std::string& w,
                    const std::string& out);
    std::pair<std::string, std::vector<Int>> VsPad(const std::string& key, const std::string& src);
    void Publish(const std::string& key, const std::string& out);
    absl::Status WriteImage(const std::string& image, const std::vector<std::pair<std::string, std::string>>& files,
                            const std::map<std::string, std::string>& bytes);
    void WriteTables(const std::vector<Result>& res);

    const BuildOptions& o_;
    std::mutex print_lock_;
    std::string chain_, out_, image_, cache_, work_, tables_;
    int nj_ = 1;
    // the profile
    std::string game_id_, inputs_, profile_rows_;
    std::map<std::string, std::pair<std::string, Int>> loc_;  // key: (package, index)
    std::map<std::string, std::string> pc_of_, map_rows_, conflict_rows_;
    std::map<std::string, Json> facts_;
    std::map<std::string, std::string> facts_rows_;
    std::map<std::string, std::set<std::string>> pairs_;
    std::set<std::string> bf_want_, dual_want_;
    // the tools
    std::unique_ptr<Metal> metal_;
    std::string tool_fp_, code_fp_;
    // the packages
    std::mutex packs_lock_;
    std::map<std::string, std::unique_ptr<Package>> packs_;
    // per shader
    std::map<std::string, std::string> digests_;
    std::set<std::string> hits_;
    std::mutex state_lock_;
    std::map<std::string, Job> jobs_;
    std::map<std::string, std::string> xerr_;
    Stats stats_;
    std::atomic<int> tmp_{0};
};

absl::Status Builder::LoadProfile() {
    const stdfs::path dir(o_.profile);
    std::optional<std::string> game = ReadOpt(dir / "game.tsv");
    if (!game)
        return absl::NotFoundError(
            absl::StrCat(o_.profile, ": no game.tsv (the game's profile folder, nxnative's dm/games/<id>)"));
    game_id_ = stdfs::absolute(dir).lexically_normal().filename().string();
    if (game_id_.empty()) game_id_ = stdfs::absolute(dir).lexically_normal().parent_path().filename().string();
    // dmsh_chain.profile_rows: the nvn_* and shader_* keys, as game.py reads its rows
    for (const std::string& line : py::SplitLines(*game)) {
        if (py::Strip(line).empty() || py::StartsWith(py::LStrip(line), "#")) continue;
        std::vector<std::string> c = py::Split(line, "\t");
        const std::string k(py::Strip(c[0]));
        if (!py::StartsWith(k, "nvn_") && !py::StartsWith(k, "shader_")) continue;
        std::vector<std::string> v;
        for (std::size_t i = 1; i < c.size(); ++i) v.emplace_back(py::Strip(c[i]));
        absl::StrAppend(&profile_rows_, k, "\t", absl::StrJoin(v, "\t"), "\n");
    }
    // dmsh_chain.inputs: the digest of the shader tables and those keys
    std::string h;
    for (const std::string& f : kInputTables) {
        std::optional<std::string> t = ReadOpt(dir / f);
        absl::StrAppend(&h, "game/", f, std::string_view("\0", 1), t ? *t : "missing", std::string_view("\0", 1));
    }
    h += profile_rows_;
    inputs_ = Sha256Hex(h).substr(0, 12);
    const std::optional<std::string> map = ReadOpt(dir / "shader_map.tsv");
    if (!map)
        return absl::NotFoundError(
            absl::StrCat("[Errno 2] No such file or directory: '", (dir / "shader_map.tsv").string(), "'"));
    const std::optional<std::string> conflicts = ReadOpt(dir / "shader_map.tsv.conflicts.tsv");
    if (!conflicts)
        return absl::NotFoundError(absl::StrCat("[Errno 2] No such file or directory: '",
                                                (dir / "shader_map.tsv.conflicts.tsv").string(), "'"));
    // MAP_ROWS (every row of five columns, raw), then pc_of and loc (rows of a package)
    for (const std::string& raw : py::Split(*map, "\n")) {
        if (raw.empty()) continue;
        const std::vector<std::string> c = py::Split(raw, "\t");
        if (c.size() > 4 && !map_rows_.count(c[3])) map_rows_[c[3]] = raw + "\n";
        if (c.size() > 4 && py::EndsWith(c[0], ".bin")) {
            if (!pc_of_.count(c[3])) pc_of_[c[3]] = c[4];
            if (!loc_.count(c[3])) loc_[c[3]] = {c[0], py::ToInt(c[1])};
        }
    }
    for (const std::string& raw : py::Split(*conflicts, "\n")) {
        if (raw.empty()) continue;
        const std::vector<std::string> c = py::Split(raw, "\t");
        if (c.size() > 1 && !py::StartsWith(raw, "#") && !py::StartsWith(raw, "dm_")) {
            if (!conflict_rows_.count(c[0])) conflict_rows_[c[0]] = raw + "\n";
            if (pc_of_.count(c[0])) pc_of_[c[0]] = c[1];
        }
    }
    // the answer table
    if (std::optional<std::string> t = ReadOpt(dir / "shader_facts.tsv")) {
        for (const std::string& l : py::Split(*t, "\n")) {
            if (py::Strip(l).empty() || py::StartsWith(l, "#")) continue;
            std::vector<std::string> kv = py::Split(l, "\t", 1);
            if (kv.size() < 2)
                return absl::InvalidArgumentError(absl::StrCat("shader_facts.tsv: a row without its facts: ", l));
            absl::StatusOr<Json> j = JsonParse(kv[1]);
            if (!j.ok())
                return absl::InvalidArgumentError(
                    absl::StrCat("shader_facts.tsv: ", kv[0], ": ", j.status().message()));
            facts_[kv[0]] = *j;
            facts_rows_[kv[0]] = absl::StrCat(kv[0], "\t", JsonSortedCompact(*j, false), "\n");
        }
    } else {
        return absl::FailedPreconditionError(
            absl::StrCat("[RED] no answer table ", (dir / "shader_facts.tsv").string(),
                         " (it comes with the game profile; an empty table = every query answered with defaults)"));
    }
    // both pair tables (load_pairs): pso_pairs.tsv from the profile, else the split's copy beside the package folder
    for (const char* table : {"shader_pairs.tsv", "pso_pairs.tsv"}) {
        stdfs::path p = dir / table;
        std::error_code ec;
        if (std::string_view(table) == "pso_pairs.tsv" && !stdfs::is_regular_file(p, ec))
            p = stdfs::absolute(o_.packages).lexically_normal().parent_path() / table;
        if (std::string_view(table) == "pso_pairs.tsv" && !stdfs::is_regular_file(p, ec)) continue;
        std::optional<std::string> t = ReadOpt(p);
        if (!t) return absl::NotFoundError(absl::StrCat("[Errno 2] No such file or directory: '", p.string(), "'"));
        for (const std::string& ln : py::Split(*t, "\n")) {
            if (py::Strip(ln).empty() || py::StartsWith(ln, "#")) continue;
            const std::vector<std::string> vp = py::SplitWs(ln);
            if (vp.size() != 2)
                return absl::InvalidArgumentError(absl::StrCat(p.string(), ": a row that is not two keys: ", ln));
            pairs_[vp[0]].insert(vp[1]);
        }
    }
    for (const auto& [f, into] : {std::pair{"bf_want.tsv", &bf_want_}, std::pair{"dual_want.tsv", &dual_want_}}) {
        if (std::optional<std::string> t = ReadOpt(dir / f)) {
            for (const std::string& ln : py::Split(*t, "\n")) {
                if (py::Strip(ln).empty() || py::StartsWith(py::LStrip(ln), "#")) continue;
                into->insert(std::string_view(f) == "bf_want.tsv" ? std::string(py::Strip(ln)) : py::SplitWs(ln)[0]);
            }
        }
    }
    if (std::optional<std::string> t = ReadOpt(dir / "geo_mesh.tsv")) {
        for (const std::string& ln : py::SplitLines(*t)) {
            if (!py::Strip(ln).empty() && !py::StartsWith(ln, "#")) {
                Print(
                    "[RED] geo_mesh.tsv lists geometry shaders drawn as mesh shaders: their libraries (dmsh_mesh.py) "
                    "are not ported to nxmac yet");
                break;
            }
        }
    }
    return absl::OkStatus();
}

absl::Status Builder::CheckTools() {
    std::error_code ec;
    if (!stdfs::is_regular_file(o_.xlate, ec))
        return absl::FailedPreconditionError(
            absl::StrCat("[RED] translation chain tools do not match the lock file: dmsh_xlate: lock ", kXlateBinary,
                         " / actual missing (", o_.xlate, ")"));
    ProcessResult r = RunProcess({o_.xlate, "--source-sha"});
    const std::string embedded(py::Strip(r.out));
    if (r.code != 0 || embedded.empty()) {
        const std::string first = !py::Strip(r.err).empty() ? py::SplitLines(py::Strip(r.err))[0]
                                  : !r.error.empty()        ? r.error
                                                            : absl::StrCat("exit code ", r.code);
        return absl::FailedPreconditionError(
            absl::StrCat("[RED] the translator does not run (", first, "): ", o_.xlate,
                         " is nxnative's committed native executable (dm/tools/native_tools.sh builds it)"));
    }
    if (embedded != kXlateSource) {
        return absl::FailedPreconditionError(absl::StrCat(
            "[RED] translation chain tools do not match the lock file: xlate_source: lock ", kXlateSource, " / actual ",
            embedded, " (nxmac ports the chain ", abi::kChain, "; a translator of another chain makes other shaders)"));
    }
    absl::StatusOr<std::string> bin = ReadFile(o_.xlate);
    const std::string xlate_sha = bin.ok() ? Sha256Hex(*bin) : "";
    if (xlate_sha.substr(0, 16) != kXlateBinary) {
        Print(absl::StrCat("[RED] the translator is not nxnative's committed build (dmsh_xlate ",
                           xlate_sha.substr(0, 16), ", the lock file has ", kXlateBinary,
                           "): it is built from the same source, so it should translate alike"));
    }
    if (metal_->version() != kLockMetal) {
        Print(absl::StrCat("note: the Metal compiler is \"", metal_->version(), "\", nxnative's lock file has \"",
                           kLockMetal, "\": the libraries are what this one compiles"));
    }
    tool_fp_ =
        absl::StrCat("xlate\0"sv, xlate_sha, "\0source\0"sv, embedded, "\0spirv-cross\0"sv, kSpirvCross, "\0xc\0"sv,
                     absl::StrJoin(kXc, " "), "\0"sv, metal_->Fingerprint(), "\0options\0debug=True reduced=True"sv);
    return absl::OkStatus();
}

std::string Builder::RecordOf(const std::string& key) {
    const std::string& name = loc_.at(key).first;
    Package* p = nullptr;
    {
        std::lock_guard<std::mutex> l(packs_lock_);
        auto it = packs_.find(name);
        if (it == packs_.end()) {
            absl::StatusOr<std::unique_ptr<Package>> opened = Package::Open((stdfs::path(o_.packages) / name).string());
            if (!opened.ok()) throw py::Fatal(std::string(opened.status().message()));
            it = packs_.emplace(name, std::move(*opened)).first;
        }
        p = it->second.get();
    }
    auto r = p->records().find(key);
    if (r == p->records().end()) {
        bool all_zero = !p->records().empty();
        for (const auto& [k, v] : p->records()) all_zero = all_zero && k == "0000000000000000";
        throw py::Fatal(absl::StrCat(
            "[RED] ", name, " holds no record of shader ", key,
            ", which the shader map names: the package and the map come from different shader_split.py runs",
            all_zero ? " (its records carry no keys: it was split before they did)" : "", "; split the shaders again"));
    }
    return std::string(p->view().substr(r->second.first, r->second.second - r->second.first));
}

std::string Builder::ShaderKey(const std::string& key, const std::string& name, const std::string& rec) {
    (void)name;
    const std::string nul(1, '\0');
    const std::string pc = facts_rows_.count(key) ? facts_rows_.at(key) : "missing";
    const std::string map_row = (map_rows_.count(key) ? map_rows_.at(key) : "missing") +
                                (conflict_rows_.count(key) ? conflict_rows_.at(key) : "");
    std::vector<std::string> pairs;
    if (auto it = pairs_.find(key); it != pairs_.end()) pairs.assign(it->second.begin(), it->second.end());
    const std::string payload =
        absl::StrCat("nxmac shader\0record\0"sv, rec, nul, "map\0"sv, map_row, nul, "pc\0"sv, pc, nul, "tools\0"sv,
                     tool_fp_, nul, "code\0"sv, code_fp_, nul, "chain\0"sv, abi::kChain, nul, "profile\0"sv,
                     profile_rows_, nul, "pairs\0"sv, absl::StrJoin(pairs, ","), nul, "dual\0"sv,
                     dual_want_.count(key) ? "1" : "0", nul, "bf\0"sv, bf_want_.count(key) ? "1" : "0");
    return Sha256Hex(payload);
}

std::optional<std::string> Builder::EntryDir(const std::string& key) const {
    auto d = digests_.find(key);
    if (d == digests_.end()) return std::nullopt;
    const stdfs::path p = stdfs::path(cache_) / "shader" / d->second.substr(0, 2) / d->second;
    std::error_code ec;
    if (!stdfs::is_regular_file(p / (key + "_b.json"), ec)) return std::nullopt;
    return p.string();
}

std::optional<std::string> Builder::Output(const std::string& key, const std::string& name) const {
    std::optional<std::string> d = EntryDir(key);
    if (!d) return std::nullopt;
    return ReadOpt(stdfs::path(*d) / name);
}

// ---- job_for and what it reads of a record

std::optional<std::pair<std::string, std::string>> VertexAb(const std::string& key, const std::string& code,
                                                            std::string_view ctl) {
    if (code.size() < 0x80 || ctl.size() < 0x760 || code.substr(0, 4) != std::string("\x78\x56\x34\x12", 4))
        return std::nullopt;
    const std::uint32_t w0 = U32(code, 0x30);
    if ((w0 & 0x1f) != 1 || ((w0 >> 10) & 0xf) != 0) return std::nullopt;
    const Int a_sph = U32(ctl, 0x748), a_len = U32(ctl, 0x74c), b_sph = U32(ctl, 0x754), b_len = U32(ctl, 0x758);
    if (!a_sph && !a_len && !b_sph && !b_len) return std::nullopt;
    const Int n = static_cast<Int>(code.size());
    const std::uint32_t bw0 = 0x80 <= b_sph && b_sph <= n - 0x50 ? U32(code, static_cast<std::size_t>(b_sph)) : 0;
    auto mod32 = [](Int v) { return ((v % 32) + 32) % 32; };
    if (a_sph != 0x30 || b_sph != static_cast<Int>(U32(ctl, 0x700)) + 0x30 || (bw0 & 0x1f) != 1 ||
        ((bw0 >> 10) & 0xf) != 1 || mod32(a_len - 0x50) || mod32(b_len - 0x50) || b_sph + b_len > n) {
        throw py::Fatal(
            absl::StrFormat("[RED] %s: vertex shader of shader type 0 without a VertexB part in the expected place "
                            "(control +0x748 %s %s %s %s)",
                            key, py::Hex(a_sph), py::Hex(a_len), py::Hex(b_sph), py::Hex(b_len)));
    }
    std::uint32_t sa[20], sb[20], sph[20];
    for (int i = 0; i < 20; ++i) {
        sa[i] = U32(code, static_cast<std::size_t>(a_sph) + 4 * i);
        sb[i] = U32(code, static_cast<std::size_t>(b_sph) + 4 * i);
    }
    sph[0] = sb[0] | (sa[0] & ~(0xfu << 10));
    for (int i : {1, 2, 3}) sph[i] = std::max(sa[i] & 0xffffff, sb[i] & 0xffffff) | (sb[i] & ~0xffffffu);
    sph[4] = sb[4];
    for (int i = 5; i < 20; ++i) sph[i] = sa[i] | sb[i];
    std::string a(py::Slice(code, a_sph + 0x50, a_sph + a_len));
    const std::string b(py::Slice(code, b_sph + 0x50, b_sph + b_len));
    for (std::size_t o = 0; o < a.size(); o += 8) {
        if (o % 32 == 0) continue;
        const std::uint64_t ins = U64(a, o);
        if ((ins >> 52) == 0xe30 && (ins & 0x1f) == 0xf) {
            const std::uint64_t bra = std::uint64_t{0xe24} << 52 |
                                      ((static_cast<std::uint64_t>(a.size() - (o + 8)) & 0xffffff) << 20) |
                                      (ins & 0xfffff);
            std::memcpy(a.data() + o, &bra, 8);
        }
    }
    std::string out = code.substr(0, 0x30);
    out.append(reinterpret_cast<const char*>(sph), sizeof sph);
    out += a + b;
    return std::pair{out, std::string(py::From(code, b_sph + b_len))};
}

std::optional<Json> Builder::TfLayout(const std::string& key, std::string_view ctl) {
    std::uint16_t strides[4];
    for (int b = 0; b < 4; ++b) strides[b] = U16(ctl, 0x6dc + 2 * b);
    if (!strides[0] && !strides[1] && !strides[2] && !strides[3]) return std::nullopt;
    Json out = Json::MakeArray();
    for (int b = 0; b < 4; ++b) {
        if (!strides[b]) {
            out.push_back(Json::MakeArray());
            continue;
        }
        if (0x6e8u + b >= ctl.size()) throw py::Fatal("[RED] index out of range");
        const int count = static_cast<unsigned char>(ctl[0x6e8 + b]);
        const std::string_view locs = py::Slice(ctl, 0xdc + 0x80 * b, 0xdc + 0x80 * b + count);
        const std::string_view sizes = py::Slice(ctl, 0x4dc + 0x80 * b, 0x4dc + 0x80 * b + count);
        bool bad = !(1 <= count && count <= 128);
        int sum = 0;
        for (char x : locs)
            bad = bad || !(0x20 <= static_cast<unsigned char>(x) && static_cast<unsigned char>(x) < 0x40);
        for (char x : sizes) {
            bad = bad || !(1 <= static_cast<unsigned char>(x) && static_cast<unsigned char>(x) <= 4);
            sum += static_cast<unsigned char>(x);
        }
        if (bad || strides[b] != 4 * sum) {
            auto list = [](std::string_view v) {
                std::vector<std::string> s;
                for (char x : v) s.push_back(absl::StrCat(static_cast<unsigned char>(x)));
                return absl::StrCat("[", absl::StrJoin(s, ", "), "]");
            };
            Print(
                absl::StrFormat("[RED] %s: transform feedback buffer %d has a layout of unknown form (stride %d, "
                                "outputs %s, components %s): no capture entry point; see tf_layout",
                                key, b, strides[b], list(locs), list(sizes)));
            return std::nullopt;
        }
        Json e = Json::MakeObject();
        e.set("stride", static_cast<int>(strides[b]));
        Json outs = Json::MakeArray();
        for (std::size_t i = 0; i < locs.size() && i < sizes.size(); ++i) {
            Json pair = Json::MakeArray();
            pair.push_back(static_cast<int>(static_cast<unsigned char>(locs[i])) - 0x20);
            pair.push_back(static_cast<int>(static_cast<unsigned char>(sizes[i])));
            outs.push_back(pair);
        }
        e.set("outputs", outs);
        out.push_back(e);
    }
    return out;
}

std::string VsTf(const std::string& src, const Json& tf) {
    auto m = re::search(R"(\bvertex\s+main0_out\s+main0\s*\((.*?)\)\s*\{)", src, re::S);
    if (!m)
        throw py::Fatal(
            "[RED] vertex shader without a main0 main function, cannot add the transform feedback entry point");
    const std::string params(m->group(1)), body = src.substr(m->end());
    auto vm = re::search(R"(uint\s+(\w+)\s*\[\[vertex_id\]\])", params);
    auto im = re::search(R"(uint\s+(\w+)\s*\[\[instance_id\]\])", params);
    const std::string vid = vm ? std::string(vm->group(1)) : "dm_vid", iid = im ? std::string(im->group(1)) : "dm_iid";
    auto st = re::search(R"(struct main0_out\s*\{(.*?)\};)", src, re::S);
    std::map<Int, std::string> names;
    static const re::Pattern user(R"(float4 (\w+) \[\[user\(locn(\d+)\)\]\])");
    for (const re::Match& u : user.finditer(st ? st->group(1) : std::string_view()))
        names[py::ToInt(u.group(2))] = u.group(1);
    std::vector<std::string> stores;
    for (std::size_t b = 0; b < tf.size(); ++b) {
        const Json& e = tf.array()[b];
        if (!e.truthy()) continue;
        Int offset = 0;
        for (const Json& on : e.at("outputs").array()) {
            const Int o = on.array()[0].as_int(), n = on.array()[1].as_int();
            for (Int i = 0; i < n; ++i) {
                const char c = "xyzw"[i];
                const std::string value =
                    names.count(o) ? absl::StrCat("as_type<uint>(out.", names[o], ".", std::string(1, c), ")")
                                   : (c == 'w' ? "0x3f800000u" : "0u");
                stores.push_back(absl::StrFormat("dm_tf%d[dm_tf.offset[%d] / 4 + dm_linear * %d + %d] = %s;", b, b,
                                                 e.at("stride").as_int() / 4, offset + i, value));
            }
            offset += n;
        }
    }
    std::string extra =
        absl::StrCat(vm ? "" : ", uint dm_vid [[vertex_id]]", im ? "" : ", uint dm_iid [[instance_id]]");
    for (std::size_t b = 0; b < tf.size(); ++b)
        if (tf.array()[b].truthy()) absl::StrAppend(&extra, ", device uint* dm_tf", b, " [[buffer(", 8 + b, ")]]");
    extra += ", constant dm_tf_capture& dm_tf [[buffer(14)]]";
    const std::string pro =
        absl::StrCat("uint dm_linear = (", iid, " - dm_tf.base_instance) * dm_tf.count + ", vid, " - dm_tf.first;\n");
    const std::string decl =
        "\nstruct dm_tf_capture { uint4 offset; uint4 args; device const uint* order; uint first, count, "
        "base_instance, pad; ulong reserved; };\n";
    return absl::StrCat(
        decl, "\nvertex void main0_tf(", params, extra, ")\n{\n", pro,
        py::Replace(body, "return out;",
                    absl::StrCat("{ if (dm_linear < dm_tf.args.x) { ", absl::StrJoin(stores, " "), " } return; }")));
}

bool EarlyFragmentTests(std::string_view ctl, std::string_view code) {
    const bool flag = ctl.size() > 0x718 && ctl[0x718] != 0;
    return flag && code.size() >= 0x50 && !(U32(code, 19 * 4) & 2);
}

Job Builder::JobFor(const std::string& key, const std::string& name, const std::string& rec, const std::string& w) {
    const Int ci = py::Find(rec, kCtlMagic, 0x38);
    std::string code(py::Slice(rec, 0x38, ci));
    std::string stage = name.substr(0, 2);
    const std::string_view ctl = py::From(rec, ci);
    Json extra = Json::MakeObject();
    if (ctl.size() >= 0x48 && U32(ctl, 0) == 0x98761234 && U32(ctl, 4) == 0xc9)
        throw py::ValueError("an NVN 2 (Switch 2) shader: its build (dmsh_ampere.py) is not ported to nxmac yet");
    const Int c1o = U32(rec, static_cast<std::size_t>(ci) + 0x700),
              c1t = U32(rec, static_cast<std::size_t>(ci) + 0x704);
    std::string c1(py::Slice(rec, 0x38 + c1o, 0x38 + c1t));
    if (stage == "vs") {
        if (auto ab = VertexAb(key, code, ctl)) {
            code = ab->first;
            c1 = ab->second;
        }
    }
    if (stage != "cs") {
        code = std::string(py::From(code, 0x30));
    } else if (code.substr(0, 4) == std::string("\x79\x56\x34\x12", 4)) {
        const std::string_view pad = py::Slice(code, 0x30, 0x100);
        if (pad.find_first_not_of('\0') != std::string_view::npos)
            throw py::Fatal(absl::StrCat(
                "[RED] ", key,
                ": compute code area +0x30..+0x100 is not all zero, so the instructions do not start at +0x100"));
        code = std::string(py::From(code, 0x100));
    }
    if (stage == "vs" || stage == "gs") {
        if (auto tf = TfLayout(key, ctl)) extra.set("tf", *tf);
    }
    if (stage == "ps" && EarlyFragmentTests(ctl, code)) extra.set("early_fragment_tests", true);
    const std::string cp = (stdfs::path(w) / (key + ".bin")).string();
    if (!WriteFileAtomic(cp, code).ok()) throw py::Fatal(absl::StrCat("[RED] ", cp, ": cannot be written"));
    Json j = Json::MakeObject();
    j.set("key", key);
    j.set("stage", stage);
    j.set("code", cp);
    j.set("tex", Json::MakeObject());
    j.set("img", Json::MakeObject());
    j.set("attr", Json::MakeObject());
    j.set("fout", Json::MakeObject());
    j.set("c1", absl::BytesToHexString(c1));
    j.update(extra);
    j.set("debug", true);
    j.set("reduced", true);
    if (!facts_.count(key)) {
        j.set("no_facts", true);
    } else {
        j.update(facts_.at(key));
    }
    return Job{j, c1};
}

// ---- the translator

std::string Builder::Translate(const std::vector<std::string>& batch, int n) {
    const std::string wb = (stdfs::path(work_) / absl::StrCat("batch_", n)).string();
    std::error_code ec;
    stdfs::create_directories(wb, ec);
    std::vector<std::string> miss;
    for (const std::string& k : batch)
        if (!hits_.count(k)) miss.push_back(k);
    std::vector<std::string> failed;
    for (const std::string& k : miss) {
        try {
            Job job = JobFor(k, loc_.at(k).first, RecordOf(k), wb);
            std::lock_guard<std::mutex> l(state_lock_);
            jobs_[k] = std::move(job);
        } catch (const py::ValueError& e) {  // a shader whose kind is not ported: reported with it
            std::lock_guard<std::mutex> l(state_lock_);
            xerr_[k] = e.what();
            failed.push_back(k);
        }
    }
    std::vector<std::vector<std::string>> chunks(static_cast<std::size_t>(nj_));
    std::size_t m = 0;
    for (const std::string& k : miss) {
        if (std::find(failed.begin(), failed.end(), k) != failed.end()) continue;
        chunks[m++ % static_cast<std::size_t>(nj_)].push_back(k);
    }
    // miss[i::NJ]: the chunks of the Python, taken without the shaders it never translates
    chunks.erase(std::remove_if(chunks.begin(), chunks.end(), [](const auto& c) { return c.empty(); }), chunks.end());
    std::vector<std::string> errs(chunks.size());
    ParallelFor(chunks.size(), nj_, [&](std::size_t i) {
        Json list = Json::MakeArray();
        {
            std::lock_guard<std::mutex> l(state_lock_);
            for (const std::string& k : chunks[i]) list.push_back(jobs_.at(k).j);
        }
        const std::string jp = (stdfs::path(wb) / absl::StrCat("job_", i, ".json")).string();
        (void)WriteFileAtomic(jp, JsonLine(list));
        ProcessResult r = RunProcess({o_.xlate, jp, wb});
        errs[i] = r.err + (r.error.empty() ? "" : r.error + "\n");
    });
    static const re::Pattern red(R"(\[RED\] (\w+): (.*))");
    for (const std::string& e : errs) {
        for (const std::string& ln : py::SplitLines(e)) {
            if (auto x = red.match(ln)) {
                std::lock_guard<std::mutex> l(state_lock_);
                xerr_[std::string(x->group(1))] = std::string(py::To(x->group(2), 160));
            }
        }
    }
    return wb;
}

// ---- finish

std::pair<std::string, std::vector<Int>> Builder::VsPad(const std::string& key, const std::string& src) {
    static const re::Pattern locn(R"(\[\[user\(locn(\d+)\))");
    std::set<Int> want;
    if (auto it = pairs_.find(key); it != pairs_.end()) {
        for (const std::string& ps : it->second) {
            if (std::optional<std::string> data = Output(ps, ps + "_b.metal"))
                for (const std::string& x : locn.findall(*data)) want.insert(py::ToInt(x));
        }
    }
    std::set<Int> have;
    for (const std::string& x : locn.findall(src)) have.insert(py::ToInt(x));
    std::vector<Int> miss;
    for (Int l : want)
        if (!have.count(l)) miss.push_back(l);
    const Int i = py::Find(src, "struct main0_out\n{\n");
    const Int j = py::Find(src, "\n};", i);
    const Int k = py::Find(src, "main0_out out = {};");
    if (miss.empty() || i < 0 || j < 0 || k < 0) return {src, {}};
    std::string decls, inits;
    for (Int l : miss) {
        absl::StrAppend(&decls, "\n    float4 dm_pad", l, " [[user(locn", l, ")]];");
        absl::StrAppend(&inits, "\n    out.dm_pad", l, " = float4(0.0, 0.0, 0.0, 1.0);");
    }
    std::string out = absl::StrCat(py::To(src, j), decls, py::From(src, j));
    const Int k2 =
        py::Find(out, "main0_out out = {};") + static_cast<Int>(std::string_view("main0_out out = {};").size());
    return {absl::StrCat(py::To(out, k2), inits, py::From(out, k2)), miss};
}

void Builder::VinFinish(const std::string& key, const std::string& full, Json& refl, const std::string& w,
                        const std::string& out) {
    if (refl.at("stage").as_string() != "vs") return;
    const std::string lib = (stdfs::path(out) / (key + "_bv.metallib")).string();
    refl.erase("vin");
    std::error_code ec;
    auto [v, shadows] = vin::Variant(full);
    if (!v) {
        if (shadows.as_string() != "no integer input") {
            stats_.Add(stats_.vin, "not generated");
            Print(absl::StrCat("[RED] ", key, ": no shadow-input library (", shadows.as_string(),
                               "): its integer inputs keep the CPU conversion"));
        }
        stdfs::remove(lib, ec);
        return;
    }
    stats_.Add(stats_.vin, "generated");
    const std::string msl = (stdfs::path(w) / (key + "_bv.metal")).string();
    (void)WriteFileAtomic(msl, *v);
    const absl::Status s = metal_->Compile(msl, lib);
    stdfs::remove(msl, ec);
    if (!s.ok()) {
        stats_.Add(stats_.vin, "failed");
        Print(absl::StrCat("[RED] ", key, ": the shadow-input library does not compile (", py::To(s.message(), 120),
                           "): its integer inputs keep the CPU conversion"));
        stdfs::remove(lib, ec);
        return;
    }
    Json vin = Json::MakeObject();
    vin.set("entry", "main0");
    vin.set("entry_z", "main0_z");
    vin.set("shadow", shadows);
    refl.set("vin", vin);
    int float_only = 0;
    for (const Json& sh : shadows.array()) float_only += !sh.contains("sint") && !sh.contains("uint");
    stats_.Add(stats_.vin, "inputs", static_cast<int>(shadows.size()));
    stats_.Add(stats_.vin, "float only", float_only);
}

void Builder::PullFinish(const std::string& key, const std::string& full, Json& refl, const std::string& w,
                         const std::string& out) {
    if (refl.at("stage").as_string() != "vs") return;
    const std::string lib = (stdfs::path(out) / (key + "_bp.metallib")).string();
    refl.erase("pull");
    std::error_code ec;
    auto [v, info] = pull::Variant(full);
    if (!v) {
        const bool none = info.is_string() && info.as_string() == "no stage input";
        stats_.Add(stats_.pull, none ? "no stage input" : "not generated");
        if (!none)
            Print(absl::StrCat("[RED] ", key, ": no pulling library (", info.as_string(),
                               "): its pipelines are always built for the draw's vertex layout"));
        stdfs::remove(lib, ec);
        return;
    }
    const std::string msl = (stdfs::path(w) / (key + "_bp.metal")).string();
    (void)WriteFileAtomic(msl, *v);
    const absl::Status s = metal_->Compile(msl, lib);
    stdfs::remove(msl, ec);
    if (!s.ok()) {
        stats_.Add(stats_.pull, "failed");
        Print(absl::StrCat("[RED] ", key, ": the pulling library does not compile (", py::To(s.message(), 120),
                           "): its pipelines are always built for the draw's vertex layout"));
        stdfs::remove(lib, ec);
        return;
    }
    stats_.Add(stats_.pull, "generated");
    Json kept = Json::MakeObject();
    for (const auto& [k, x] : info.object())
        if (!x.is_null()) kept.set(k, x);
    refl.set("pull", kept);
}

Json Builder::PsLibraries(const std::string& key, const std::string& msl, const std::string& full, const Json& bindings,
                          const std::string& w, const std::string& out) {
    auto drop = [&](const std::vector<std::string>& suffixes) {
        std::error_code ec;
        for (const std::string& s : suffixes) stdfs::remove(stdfs::path(out) / (key + s + ".metallib"), ec);
    };
    const side::Unit u = side::MakeUnit(full);
    std::set<std::string> sideset;
    if (u.src) {
        sideset.insert(u.alpha.begin(), u.alpha.end());
        sideset.insert(u.logic.begin(), u.logic.end());
    }
    const fetch::Fold folded = fetch::MakeFold(u.src ? *u.src : full, bindings, bf_want_.count(key) > 0);
    std::vector<std::pair<std::string, Json>> units;
    if (folded.src) units.emplace_back(*folded.src, folded.meta);
    if (u.src) units.emplace_back(*u.src, Json());
    for (const auto& [unit, fetchmeta] : units) {
        const bool fetch = !fetchmeta.is_null();
        std::vector<std::string> names = {"_b"};
        std::vector<std::set<std::string>> groups;
        if (!sideset.empty()) {
            names.emplace_back("_bx");
            groups.push_back(sideset);
        }
        if (fetch) {
            names.emplace_back("_bf");
            groups.push_back({"main0_f"});
        }
        const std::string um = (stdfs::path(w) / (key + "_bu.metal")).string();
        (void)WriteFileAtomic(um, unit);
        std::vector<std::string> libs;
        libs.reserve(names.size());
        for (const std::string& n : names) libs.push_back((stdfs::path(out) / (key + n + ".metallib")).string());
        const absl::Status s =
            metal_->CompileSplit(um, libs, [&](const std::string& lib) { return side::Cut(lib, groups); });
        std::error_code ec;
        stdfs::remove(um, ec);
        if (!s.ok()) {
            if (fetch) {
                stats_.Add(stats_.bf, "failed");
                Print(absl::StrCat("[RED] ", key, ": the readable-tile version does not build (",
                                   py::To(s.message(), 120), "): its draws go without the tile read"));
            } else {
                stats_.Add(stats_.side, "failed");
                Print(absl::StrCat("[RED] ", key, ": the alpha test and logic operation entry points do not build (",
                                   py::To(s.message(), 120), "): its draws go without them"));
            }
            continue;
        }
        std::vector<std::string> dropped;
        for (const char* x : {"_bx", "_bf"})
            if (std::find(names.begin(), names.end(), x) == names.end()) dropped.emplace_back(x);
        drop(dropped);
        stats_.Add(stats_.side, "generated", !sideset.empty());
        stats_.Add(stats_.bf, "generated", fetch);
        Json r = Json::MakeObject();
        if (!sideset.empty()) {
            if (!u.alpha.empty()) {
                Json e = Json::MakeObject();
                Json list = Json::MakeArray();
                for (const std::string& a : u.alpha) list.push_back(a);
                e.set("entries", list);
                r.set("alpha", e);
            }
            if (!u.logic.empty()) {
                Json e = Json::MakeObject();
                Json list = Json::MakeArray();
                for (const std::string& a : u.logic) list.push_back(a);
                e.set("entries", list);
                r.set("logic", e);
            }
        }
        if (fetch) r.set("fetch", fetchmeta);
        return r;
    }
    stats_.Add(stats_.side, "not generated", !u.src);
    drop({"_bx", "_bf"});
    const absl::Status s = metal_->Compile(msl, (stdfs::path(out) / (key + "_b.metallib")).string());
    if (!s.ok()) throw std::runtime_error(std::string(s.message()));  // CalledProcessError: the caller's
    return Json::MakeObject();
}

Result Builder::Finish(const std::string& key, const std::string& w, Job& job, const std::string& out) {
    Json& j = job.j;
    const std::string stage = j.at("stage").as_string();
    auto fail = [&](std::string st, std::string err) { return Result{key, std::move(st), std::move(err), Json()}; };
    const auto& [name, k] = loc_.at(key);
    const std::string msl = (stdfs::path(w) / (key + ".metal")).string();
    const std::string lib = (stdfs::path(out) / (key + "_b.metallib")).string();
    const std::string spv = (stdfs::path(w) / (key + ".spv")).string();
    if (stage != "vs" && stage != "ps" && stage != "cs")
        return fail("not ported", absl::StrCat("a ", stage, " shader: its stage is not ported to nxmac yet"));
    auto cross = [&]() -> std::optional<std::string> {  // spirv-cross, as dmsh_build.py runs it: None, or why it failed
        absl::StatusOr<std::string> data = ReadFile(spv);
        if (!data.ok()) return "no spv";
        const std::vector<std::string> args = atomic::CrossArgs(kXc, *data, spv);
        std::vector<std::string> argv = args;
        argv.insert(argv.end(), {spv, "--output", msl});
        const CrossResult r = SpirvCross(argv);
        if (!r.ok) return std::string(py::To(py::Strip(r.error), 160));
        return std::nullopt;
    };
    auto single_lane = [&]() {
        absl::StatusOr<std::string> data = ReadFile(spv);
        if (!data.ok()) return;
        std::string bytes = *data;
        if (spirv::SingleLane(bytes)) (void)WriteFileAtomic(spv, bytes);
    };
    if (stage == "vs") single_lane();
    if (auto e = cross()) return fail("MSL failed", *e);
    absl::StatusOr<std::string> read = ReadFile(msl);
    if (!read.ok()) return fail("MSL failed", "no output");
    std::string src = *read;
    auto write = [&](const std::string& text) { (void)WriteFileAtomic(msl, text); };
    if (stage == "vs") {  // second pass: inputs only used as integers, reported as uint
        Json extra = Json::MakeObject();
        const Json ints = entry::IntInputs(src);
        for (const auto& [l, t] : ints.object())
            if (!j.at("attr").contains(l)) extra.set(l, t);
        if (extra.size()) {
            j["attr"].update(extra);
            const std::string jp = (stdfs::path(w) / ("job1_" + key + ".json")).string();
            Json list = Json::MakeArray();
            list.push_back(j);
            (void)WriteFileAtomic(jp, JsonLine(list));
            std::error_code ec;
            stdfs::remove(spv, ec);  // the Python reads the new one, or none
            ProcessResult rx = RunProcess({o_.xlate, jp, w});
            if (rx.code != 0 || !stdfs::exists(spv, ec))
                return fail("translation failed",
                            std::string(py::To(absl::StrCat("integer-input retranslation: ", py::Strip(rx.err)), 160)));
            single_lane();
            if (auto e = cross()) return fail("MSL failed", *e);
            read = ReadFile(msl);
            if (!read.ok()) return fail("MSL failed", "no output");
            src = *read;
        }
    }
    try {
        atomic::CheckMsl(src);
    } catch (const py::ValueError& e) {
        return fail("image atomic MSL failed", e.what());
    }
    src = rewrite::Precise(src);
    auto step = [&](const char* st, const auto& f) -> std::optional<Result> {
        try {
            src = f(src);
        } catch (const py::ValueError& e) {
            return fail(st, std::string(py::To(e.what(), 160)));
        }
        return std::nullopt;
    };
    if (auto r = step("uniform bounds rewrite failed", fetch::SelectGuards)) return *r;
    src = rewrite::MulExtended(src);
    if (auto r = step("gather offsets rewrite failed", rewrite::GatherOffsets)) return *r;
    if (auto r = step("HMMA rewrite failed", rewrite::HmmaMsl)) return *r;
    if (auto r = step("shared store rewrite failed", rewrite::SharedStoreMsl)) return *r;
    if (auto r = step("pool rewrite failed", reflect::PoolArrays)) return *r;
    src = wide::WideLoads(src);
    src = rewrite::BorderEmu(src);
    if (stage == "ps") src = rewrite::FragZSnap(src).first;
    int blend0 = 0;
    if (stage == "ps") std::tie(src, blend0) = rewrite::BlendZero(src);
    write(src);
    std::vector<rewrite::FirstStore> first;
    Int spill = 0;
    if (stage == "cs") {
        const Json* local = j.find("local");
        std::tie(src, first) = rewrite::FirstStores(src, local ? *local : Json());
        write(src);
        try {
            std::tie(src, spill) = rewrite::TgSpill(src);
        } catch (const py::ValueError& e) {
            return fail("shared memory rewrite failed", std::string(py::To(e.what(), 160)));
        }
        if (spill) write(src);
    }
    std::vector<Int> pad;
    if (stage == "vs") {
        src = entry::VsBaseInstance(entry::VsLayer(entry::VsInit(src)));
        write(src);
    }
    if (stage == "vs" && pairs_.count(key) && !pairs_.at(key).empty()) {
        std::tie(src, pad) = VsPad(key, src);
        if (!pad.empty()) write(src);
    }
    Json lay;
    bool discard = false;
    absl::StatusOr<std::string> res_text = ReadFile((stdfs::path(w) / (key + ".res.json")).string());
    if (!res_text.ok()) throw py::Fatal(absl::StrCat("[RED] ", key, ": the translator wrote no ", key, ".res.json"));
    absl::StatusOr<Json> res = JsonParse(*res_text);
    if (!res.ok()) throw py::Fatal(absl::StrCat("[RED] ", key, ".res.json: ", res.status().message()));
    std::string plain;
    if (stage == "vs") {
        const std::string capture = geometry::Capture(src);
        std::string full;
        std::tie(src, full) = entry::VsDepth(src);
        full += capture;
        discard = entry::VsWrites(src, reflect::Reflect(key, "vs", src, *res));
        if (discard) full += entry::VsDiscard(src);
        if (const Json* tf = j.find("tf")) full += VsTf(src, *tf);
        plain = full;
        std::tie(full, lay) = entry::VsLayerEntry(full);
        write(full);
    }
    std::optional<std::string> fu, fd;
    Int fmask = 0;
    if (stage == "ps") std::tie(fu, fmask) = entry::FsUint(src);
    if (stage == "ps" && dual_want_.count(key)) fd = entry::FsDual(src);
    std::optional<std::string> shared;
    if (fu || fd) shared = side::SharedBody(src);
    if (shared) {
        plain = src;
        write(*shared + fu.value_or("") + fd.value_or(""));
    } else {
        fu.reset();
        fd.reset();
    }
    Json binds;
    try {
        binds = reflect::Reflect(key, stage, src, *res);
    } catch (const py::ValueError& e) {
        return fail("reflection failed", std::string(py::To(e.what(), 160)));
    }
    Json side_info = Json::MakeObject();
    auto compile_targets = [&]() -> absl::Status {
        if (stage == "ps") {
            absl::StatusOr<std::string> text = ReadFile(msl);
            try {
                side_info = PsLibraries(key, msl, text.ok() ? *text : "", binds, w, out);
            } catch (const py::ValueError&) {
                throw;
            } catch (const py::Fatal&) {
                throw;
            } catch (const std::runtime_error& e) {
                return absl::FailedPreconditionError(e.what());
            }
            return absl::OkStatus();
        }
        return metal_->Compile(msl, lib);
    };
    std::vector<std::string> lost;
    if (absl::Status s = compile_targets(); !s.ok()) {
        if (lay.is_null() && !fu && !fd) return fail("metal failed", std::string(py::To(s.message(), 160)));
        if (!lay.is_null()) lost.emplace_back("layer");
        if (fu) lost.emplace_back("integer output");
        if (fd) lost.emplace_back("dual source");
        write(plain);
        lay = Json();
        fu.reset();
        fd.reset();
        if (absl::Status s2 = compile_targets(); !s2.ok())
            return fail("metal failed", std::string(py::To(s2.message(), 160)));
    }
    {
        std::error_code ec;
        stdfs::copy_file(msl, stdfs::path(out) / (key + "_b.metal"), stdfs::copy_options::overwrite_existing, ec);
        if (ec) throw py::Fatal(absl::StrCat("[RED] ", out, ": ", ec.message()));
    }
    auto entry_m = re::search(R"((vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\()", src);
    const Json dr = reflect::DrvReads(src);
    Json refl = Json::MakeObject();
    refl.set("key", key + "_b");
    refl.set("route", "B");
    refl.set("chain", abi::kChain);
    refl.set("dm_ctl", key);
    refl.set("source", absl::StrCat(name, "#", k));
    refl.set("pc_paired", pc_of_.at(key));
    refl.set("stage", stage);
    refl.set("entry", entry_m ? Json(entry_m->group(2)) : Json("main0"));
    refl.set("msl_entry_kind", entry_m ? Json(entry_m->group(1)) : Json());
    if (stage == "vs") {
        refl.set("entry_z", "main0_z");
        refl.set("entry_geometry_capture", "main0_gs");
        if (entry::LayerOut(src)) refl.set("layer_out", true);
    }
    if (discard) refl.set("entry_discard", "main0_discard");
    if (const Json* tf = j.find("tf")) {
        refl.set("entry_tf", "main0_tf");
        Json strides = Json::MakeArray();
        for (const Json& e : tf->array()) strides.push_back(e.truthy() ? Json(e.at("stride").as_int()) : Json(0));
        refl.set("tf_strides", strides);
    }
    if (!lay.is_null()) {
        refl.set("entry_layer", "main0_l");
        refl.set("entry_layer_z", "main0_lz");
        refl.set("layer_from", lay);
    }
    if (fu) refl.set("entry_u", "main0_u");
    if (fu && fmask) {
        refl.set("entry_m", "main0_m");
        refl.set("uint_mask_m", fmask);
    }
    if (fd) refl.set("entry_d", "main0_d");
    if (blend0) refl.set("blend_zero", true);
    if (!lost.empty()) {
        Json l = Json::MakeArray();
        for (const std::string& x : lost) l.push_back(x);
        refl.set("entries_lost", l);
    }
    if (!first.empty()) {
        Json fs_list = Json::MakeArray();
        for (const rewrite::FirstStore& f : first) {
            Json reg;
            bool found = false;
            for (const Json& b : binds.array()) {
                if (b.at("msl_name").as_string() == f.name) {
                    const Json* r = b.find("reg");
                    if (!r) throw py::Fatal(absl::StrCat("[RED] ", key, ": KeyError: 'reg'"));
                    reg = *r;
                    found = true;
                    break;
                }
            }
            if (!found) throw py::Fatal(absl::StrCat("[RED] ", key, ": StopIteration (a first store to no binding)"));
            Json e = Json::MakeObject();
            e.set("reg", reg);
            e.set("word", f.word);
            e.set("value", f.value);
            fs_list.push_back(e);
        }
        refl.set("first_store", fs_list);
    }
    if (spill) refl.set("tg_spill", spill);
    const Json* local = j.find("local");
    refl.set("threadgroup_size", local ? *local : Json());
    refl.set("c1", absl::BytesToHexString(job.c1));
    Json notes = res->at("notes");
    if (j.contains("no_facts")) notes.push_back("not in the answer table, queries use defaults");
    refl.set("notes", notes);
    refl.set("drv_reads", dr);
    refl.set("drv_unfillable", reflect::Unfillable(dr, stage));
    refl.set("bindings", binds);
    Json vin = Json::MakeArray();
    for (const reflect::Attribute& a : reflect::Attributes(src)) {
        Json e = Json::MakeObject();
        e.set("location", a.location);
        e.set("msl_type", a.type);
        vin.push_back(e);
    }
    refl.set("vertex_inputs", vin);
    Json raster = reflect::RasterFacts(stage, src);
    if (stage == "ps") {
        absl::StatusOr<std::string> code = ReadFile(j.at("code").as_string());
        raster.set("omap", code.ok() && code->size() >= 0x50 ? Json(static_cast<Int>(U32(*code, 18 * 4))) : Json());
    }
    refl.set("raster", raster);
    Json pads = Json::MakeArray();
    for (Int p : pad) pads.push_back(p);
    refl.set("vs_pad", pads);
    if (stage == "vs") {
        absl::StatusOr<std::string> full = ReadFile(msl);
        const std::string text = full.ok() ? *full : "";
        VinFinish(key, text, refl, w, out);
        PullFinish(key, text, refl, w, out);
    }
    refl.update(side_info);
    if (absl::Status s =
            WriteFileAtomic((stdfs::path(out) / (key + "_b.json")).string(), JsonIndent1(metal_->WithCompiler(refl)));
        !s.ok())
        throw py::Fatal(absl::StrCat("[RED] ", s.message()));
    return Result{key, "ok", "", refl};
}

void Builder::Publish(const std::string& key, const std::string& out) {
    const std::string& d = digests_.at(key);
    const stdfs::path dst = stdfs::path(cache_) / "shader" / d.substr(0, 2) / d;
    std::error_code ec;
    stdfs::create_directories(dst.parent_path(), ec);
    stdfs::remove_all(dst, ec);
    stdfs::rename(out, dst, ec);
    if (ec) {  // a cache on another volume: copied in under another name, then renamed there
        const stdfs::path tmp = dst.parent_path() / absl::StrCat(".tmp-", getpid(), "-", tmp_++);
        std::error_code copy;
        stdfs::copy(out, tmp, stdfs::copy_options::recursive, copy);
        if (!copy) stdfs::rename(tmp, dst, copy);
        if (copy) throw py::Fatal(absl::StrCat("[RED] ", dst.string(), ": ", copy.message()));
        stdfs::remove_all(out, ec);
    }
}

Result Builder::Fin(const std::string& key, const std::string& wb) {
    if (hits_.count(key)) {
        const std::optional<std::string> dir = EntryDir(key);
        absl::StatusOr<std::string> text = dir ? ReadFile((stdfs::path(*dir) / (key + "_b.json")).string())
                                               : absl::StatusOr<std::string>(absl::NotFoundError("pruned meanwhile"));
        absl::StatusOr<Json> refl = text.ok() ? JsonParse(*text) : absl::StatusOr<Json>(text.status());
        if (refl.ok()) {
            const std::string stage = refl->at("stage").as_string();
            if (stage == "vs") {
                if (refl->contains("vin")) stats_.Add(stats_.vin, "generated");
                if (refl->contains("pull")) stats_.Add(stats_.pull, "generated");
            }
            if (stage == "ps") {
                stats_.Add(stats_.side, "generated", refl->contains("alpha") || refl->contains("logic"));
                stats_.Add(stats_.bf, "generated", refl->contains("fetch"));
            }
            return Result{key, "ok", "", *refl};
        }
    }
    {
        std::lock_guard<std::mutex> l(state_lock_);
        if (auto it = xerr_.find(key); it != xerr_.end()) {
            const bool not_ported = !jobs_.count(key);
            return Result{key, not_ported ? "not ported" : "translation failed", it->second, Json()};
        }
    }
    std::error_code ec;
    if (!stdfs::exists(stdfs::path(wb) / (key + ".spv"), ec))
        return Result{key, "translation failed", "no spv", Json()};
    const std::string out = (stdfs::path(work_) / absl::StrCat("out_", key)).string();
    stdfs::remove_all(out, ec);
    stdfs::create_directories(out, ec);
    Job job;
    {
        std::lock_guard<std::mutex> l(state_lock_);
        job = jobs_.at(key);
    }
    Result r = Finish(key, wb, job, out);
    if (r.st == "ok") {
        Publish(key, out);
    } else {
        stdfs::remove_all(out, ec);
    }
    return r;
}

// ---- the library, its stamp and its tables

absl::Status Builder::WriteImage(const std::string& image,
                                 const std::vector<std::pair<std::string, std::string>>& files,
                                 const std::map<std::string, std::string>& bytes) {
    // shader_lib.write: files sorted by name, each one's data at a multiple of 16, the index, the name pool, the header
    std::map<std::string, std::pair<const std::string*, const std::string*>> all;  // name: (path, bytes)
    for (const auto& [n, p] : files) all[n] = {&p, nullptr};
    for (const auto& [n, b] : bytes) all[n] = {nullptr, &b};
    const std::string part = image + ".part";
    std::ofstream f(part, std::ios::binary | std::ios::trunc);
    if (!f) return absl::UnavailableError(absl::StrCat(part, ": cannot be written"));
    std::uint64_t off = 32;
    f.write(std::string(32, '\0').data(), 32);
    struct Ent {
        std::string name;
        std::uint64_t a, b;
    };
    std::vector<Ent> ents;
    for (const auto& [name, src] : all) {
        const std::uint64_t pad = (16 - off % 16) % 16;
        if (pad) f.write(std::string(pad, '\0').data(), static_cast<std::streamsize>(pad));
        off += pad;
        std::string data;
        if (src.second) {
            data = *src.second;
        } else {
            absl::StatusOr<std::string> d = ReadFile(*src.first);
            if (!d.ok()) return absl::UnavailableError(absl::StrCat(name, ": no bytes to write"));
            data = std::move(*d);
        }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        ents.push_back({name, off, data.size()});
        off += data.size();
    }
    std::string idx, pool;
    auto ent = [&](std::uint64_t a, std::uint64_t b, std::uint32_t name_off, std::uint16_t name_len, std::uint8_t kind,
                   std::uint8_t flags) {
        char e[24];
        std::memcpy(e, &a, 8);
        std::memcpy(e + 8, &b, 8);
        std::memcpy(e + 16, &name_off, 4);
        std::memcpy(e + 20, &name_len, 2);
        e[22] = static_cast<char>(kind);
        e[23] = static_cast<char>(flags);
        idx.append(e, 24);
    };
    ent(1, ents.size(), 0, 0, 0, 0);
    for (const Ent& e : ents) {
        ent(e.a, e.b, static_cast<std::uint32_t>(pool.size()), static_cast<std::uint16_t>(e.name.size()), 1, 0);
        pool += e.name;
    }
    f.write(idx.data(), static_cast<std::streamsize>(idx.size()));
    f.write(pool.data(), static_cast<std::streamsize>(pool.size()));
    f.seekp(0);
    char head[32];
    constexpr std::string_view kMagic = "DMROMFS1";
    std::copy(kMagic.begin(), kMagic.end(), head);
    const std::uint64_t index_size = idx.size() + pool.size(), count = ents.size() + 1;
    std::memcpy(head + 8, &off, 8);
    std::memcpy(head + 16, &index_size, 8);
    std::memcpy(head + 24, &count, 8);
    f.write(head, 32);
    f.close();
    if (!f) return absl::UnavailableError(absl::StrCat(part, ": write failed (is the disk full?)"));
    // a read of it, as the host's check: nothing is published that a reader refuses
    absl::StatusOr<fs::StoragePtr> storage = fs::OpenFileStorage(part);
    if (!storage.ok()) return storage.status();
    absl::StatusOr<fs::RomFsImage> check = fs::RomFsImage::Open(*std::move(storage));
    if (!check.ok())
        return absl::InternalError(absl::StrCat(part, ": the image does not read back: ", check.status().message()));
    std::error_code ec;
    stdfs::rename(part, image, ec);
    if (ec) return absl::UnavailableError(absl::StrCat(image, ": ", ec.message()));
    return absl::OkStatus();
}

std::string Timestamp() {  // datetime.now().astimezone().isoformat(timespec="seconds")
    return absl::FormatTime("%Y-%m-%dT%H:%M:%S%Ez", absl::Now(), absl::LocalTimeZone());
}

void Builder::WriteTables(const std::vector<Result>& res) {
    std::vector<std::pair<std::string, const Result*>> ok;
    for (const Result& r : res)
        if (r.st == "ok") ok.emplace_back(r.key, &r);
    std::sort(ok.begin(), ok.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const std::string head = absl::StrCat("# chain b-", chain_, " inputs ", inputs_, " ", Timestamp(), "\n");
    std::vector<std::string> unproven(ok.size()), rows(ok.size());
    std::vector<int> has_row(ok.size(), 0);
    std::atomic<bool> fatal{false};
    std::string fatal_msg;
    std::mutex fl;
    ParallelFor(ok.size(), nj_, [&](std::size_t i) {
        const std::string& key = ok[i].first;
        const std::optional<std::string> dir = EntryDir(key);
        if (!dir) return;
        const std::optional<std::string> source = ReadOpt(stdfs::path(*dir) / (key + "_b.metal"));
        const std::optional<std::string> json = ReadOpt(stdfs::path(*dir) / (key + "_b.json"));
        if (!source) return;
        std::optional<Int> span;
        absl::StatusOr<Json> refl = json ? JsonParse(*json) : absl::StatusOr<Json>(absl::NotFoundError(""));
        if (refl.ok()) span = guard::C1Span(*refl);
        if (guard::Unproven(*source, span)) unproven[i] = key;
        if (!refl.ok()) return;
        const Json* bindings = refl->find("bindings");
        const Json empty = Json::MakeArray();
        const Json& binds = bindings && bindings->truthy() ? *bindings : empty;
        bool any = false;
        for (const Json& b : binds.array()) {
            const Json* kind = b.find("kind");
            any = any || (kind && kind->is_string() &&
                          (kind->as_string() == abi::kKindCbuf || kind->as_string() == abi::kKindSbuf));
        }
        if (!any) return;
        try {
            const std::vector<std::string> toks = guard::ExtentTokens(*source, binds);
            if (!toks.empty()) {
                rows[i] = absl::StrCat(key, "\t", absl::StrJoin(toks, " "));
                has_row[i] = 1;
            }
        } catch (const py::Fatal& e) {
            std::lock_guard<std::mutex> l(fl);
            fatal = true;
            fatal_msg = e.what();
        }
    });
    if (fatal) throw py::Fatal(fatal_msg);
    std::string c1 = head;
    int n_unproven = 0;
    for (const std::string& k : unproven) {
        if (k.empty()) continue;
        c1 += k + "\n";
        ++n_unproven;
    }
    std::string cb = head +
                     "# shader key, then reg:bytes for each uniform slot whose reads stay inside its first bytes, and "
                     "s<reg> for each storage slot it never writes (cb_extent.py)\n";
    int slots = 0, ro = 0, n_rows = 0;
    static const re::Pattern sreg(R"(\bs\d+)");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!has_row[i]) continue;
        cb += rows[i] + "\n";
        ++n_rows;
        slots += static_cast<int>(py::Count(rows[i], ":"));
        ro += static_cast<int>(sreg.finditer(py::Split(rows[i], "\t", 1)[1]).size());
    }
    std::error_code ec;
    stdfs::create_directories(tables_, ec);
    for (const auto& [name, text] : {std::pair{"c1_full.tsv", &c1}, std::pair{"cb_extent.tsv", &cb}}) {
        if (absl::Status s = WriteFileAtomic((stdfs::path(tables_) / name).string(), *text); !s.ok())
            throw py::Fatal(absl::StrCat("[RED] ", s.message()));
    }
    Print(absl::StrCat("c1_full.tsv: not proven bounded: ", n_unproven, " shaders"));
    Print(absl::StrCat("cb_extent.tsv: bounded uniform reads: ", slots, " slots; storage slots never written: ", ro,
                       "; ", n_rows, " shaders"));
}

absl::StatusOr<BuildResult> Builder::Run() {
    chain_ = std::string(abi::kChain);
    std::error_code ec;
    if (!stdfs::is_directory(o_.packages, ec))
        return absl::NotFoundError(absl::StrCat(o_.packages, ": no package folder (the split's pkg/)"));
    out_ = o_.out;
    stdfs::create_directories(out_, ec);
    if (ec) return absl::UnavailableError(absl::StrCat(out_, ": ", ec.message()));
    image_ = (stdfs::path(out_) / absl::StrCat("b-", chain_, ".img")).string();
    cache_ = o_.cache.empty() ? (stdfs::path(out_) / "cache").string() : o_.cache;
    tables_ = o_.tables.empty() ? out_ : o_.tables;
    work_ = (stdfs::path(out_) / absl::StrCat("work-", getpid())).string();
    nj_ = o_.jobs > 0 ? o_.jobs : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    if (absl::Status s = LoadProfile(); !s.ok()) return s;
    // one game per output folder: b-<V> is the same for every game
    if (stdfs::is_regular_file(image_, ec)) {
        absl::StatusOr<fs::StoragePtr> storage = fs::OpenFileStorage(image_);
        if (storage.ok()) {
            absl::StatusOr<fs::RomFsImage> old = fs::RomFsImage::Open(*std::move(storage));
            if (old.ok()) {
                for (const fs::RomFsImageEntry& e : old->entries()) {
                    if (e.directory || e.name != kStamp) continue;
                    absl::StatusOr<fs::StoragePtr> f = old->OpenFile(e);
                    if (!f.ok()) continue;
                    std::string text(e.b, '\0');
                    if (!(*f)->Read(0, std::span(reinterpret_cast<std::uint8_t*>(text.data()), text.size())).ok())
                        continue;
                    for (const std::string& l : py::SplitLines(text)) {
                        std::vector<std::string> kv = py::Split(l, "\t", 1);
                        if (kv.size() == 2 && kv[0] == "game" && kv[1] != game_id_)
                            return absl::FailedPreconditionError(
                                absl::StrCat("[RED] ", image_, " holds the shader libraries of game ", kv[1], ", not ",
                                             game_id_, ": give each game its own output directory"));
                    }
                }
            }
        }
    }
    stdfs::remove_all(work_, ec);
    stdfs::create_directories(work_, ec);
    struct RemoveWork {
        std::string path;
        ~RemoveWork() {
            std::error_code e;
            stdfs::remove_all(path, e);
        }
    } remove_work{work_};
    absl::StatusOr<std::unique_ptr<Metal>> metal = Metal::Open(cache_, work_, o_.min_macos);
    if (!metal.ok()) return metal.status();
    metal_ = std::move(*metal);
    if (absl::Status s = CheckTools(); !s.ok()) return s;
    code_fp_ = SelfDigest();
    stdfs::create_directories(stdfs::path(cache_) / "shader", ec);
    // a full build starts from no library: a stopped build leaves none, rather than one with old shaders
    stdfs::remove(image_, ec);
    std::vector<std::string> keys;
    keys.reserve(loc_.size());
    for (const auto& [k, v] : loc_) keys.push_back(k);
    std::vector<Result> res;
    try {
        for (const std::string& key : keys) digests_[key] = ShaderKey(key, loc_.at(key).first, RecordOf(key));
        std::map<std::string, std::string> plain = digests_;
        for (const std::string& key : keys) {
            auto it = pairs_.find(key);
            if (it == pairs_.end() || it->second.empty()) continue;
            std::vector<std::string> ds;
            for (const std::string& p : it->second)
                ds.push_back(plain.count(p)  ? plain.at(p)
                             : loc_.count(p) ? ShaderKey(p, loc_.at(p).first, RecordOf(p))
                                             : "missing");
            digests_[key] = Sha256Hex(absl::StrCat(digests_[key], std::string_view("\0", 1), absl::StrJoin(ds, ",")));
        }
        for (const std::string& key : keys) {
            if (EntryDir(key)) {
                hits_.insert(key);
                stats_.Add(stats_.pre, "hit");
            } else {
                stats_.Add(stats_.pre, "miss");
            }
        }
        const std::size_t batch = 4 * static_cast<std::size_t>(nj_);
        std::vector<std::vector<std::string>> batches;
        for (int phase = 0; phase < 2; ++phase) {
            std::vector<std::string> b;
            for (const std::string& k : keys) {
                const bool paired = pairs_.count(k) > 0;
                if (paired == (phase == 1)) b.push_back(k);
            }
            for (std::size_t i = 0; i < b.size(); i += batch)
                batches.emplace_back(b.begin() + static_cast<std::ptrdiff_t>(i),
                                     b.begin() + static_cast<std::ptrdiff_t>(std::min(b.size(), i + batch)));
        }
        std::atomic<int> done{0};
        auto progress = [&] {
            const int d = ++done;
            if (d % 200 == 0 || d == static_cast<int>(keys.size()))
                Print(absl::StrCat("progress: ", d, "/", keys.size()));
        };
        // the next batch is translated while this one is finished (vertex shaders with pairs, which read their pixel
        // shaders' MSL, are finished after every other shader: their batches come last)
        std::future<std::string> next;
        auto translate = [&](std::size_t i) {
            return std::async(std::launch::async,
                              [this, &batches, i] { return Translate(batches[i], static_cast<int>(i)); });
        };
        if (!batches.empty()) next = translate(0);
        for (std::size_t i = 0; i < batches.size(); ++i) {
            const std::string wb = next.get();
            if (i + 1 < batches.size()) next = translate(i + 1);
            std::vector<Result> out(batches[i].size());
            std::string fatal;
            std::mutex fl;
            ParallelFor(batches[i].size(), nj_, [&](std::size_t n) {
                try {
                    out[n] = Fin(batches[i][n], wb);
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> l(fl);
                    if (fatal.empty()) fatal = absl::StrCat(batches[i][n], ": ", e.what());
                }
                progress();
            });
            stdfs::remove_all(wb, ec);
            if (!fatal.empty()) {
                if (next.valid()) next.wait();
                return absl::FailedPreconditionError(py::StartsWith(fatal, "[RED]") ? fatal
                                                                                    : absl::StrCat("[RED] ", fatal));
            }
            res.insert(res.end(), out.begin(), out.end());
        }
    } catch (const std::exception& e) {
        return absl::FailedPreconditionError(py::StartsWith(e.what(), "[RED]") ? std::string(e.what())
                                                                               : absl::StrCat("[RED] ", e.what()));
    }
    // the summary (dmsh_build.py's lines)
    std::map<std::string, int> st;
    std::vector<std::string> st_order;
    for (const Result& r : res) {
        if (!st.count(r.st)) st_order.push_back(r.st);
        ++st[r.st];
    }
    std::string line = absl::StrCat("shaders ", res.size(), ": ok ", st["ok"], ", ");
    std::vector<std::string> others;
    for (const std::string& k : st_order)
        if (k != "ok") others.push_back(absl::StrCat(k, " ", st[k]));
    Print(line + absl::StrJoin(others, ", "));
    Print(absl::StrCat("shader cache: hit ", stats_.pre["hit"], ", rebuilt ", stats_.pre["miss"]));
    Print(absl::StrCat("readable-tile versions (<key>_bf, dmsh_fetch.py): ", stats_.bf["generated"], " generated, ",
                       stats_.bf["failed"], " failed to compile"));
    Print(absl::StrCat("shadow-input libraries (<key>_bv, dmsh_vin.py): ", stats_.vin["generated"], " generated, ",
                       stats_.vin["failed"], " failed to compile, ", stats_.vin["not generated"], " not generated; ",
                       stats_.vin["inputs"], " integer inputs shadowed, ", stats_.vin["float only"],
                       " of them with no other-sign shadow (attribute indices ran out)"));
    Print(absl::StrCat("alpha test and logic operation libraries (<key>_bx, dmsh_side.py): ", stats_.side["generated"],
                       " generated, ", stats_.side["failed"], " failed to compile, ", stats_.side["not generated"],
                       " pixel shaders without either"));
    Print(absl::StrCat("pulling libraries (<key>_bp, dmsh_pull.py): ", stats_.pull["generated"], " generated, ",
                       stats_.pull["failed"], " failed to compile, ", stats_.pull["not generated"], " not generated, ",
                       stats_.pull["no stage input"], " vertex shaders without stage inputs (none needed)"));
    Print(metal_->Summary());
    for (const std::string& lvl : st_order) {
        if (lvl == "ok") continue;
        std::map<std::string, int> errs;
        std::vector<std::string> order;
        for (const Result& r : res) {
            if (r.st != lvl) continue;
            if (!errs.count(r.err)) order.push_back(r.err);
            ++errs[r.err];
        }
        std::stable_sort(order.begin(), order.end(),
                         [&](const std::string& a, const std::string& b) { return errs[a] > errs[b]; });
        for (std::size_t i = 0; i < order.size() && i < 10; ++i)
            Print(absl::StrFormat("  [%s] %5d  %s", lvl, errs[order[i]], order[i]));
    }
    // _b_build.json, the stamp, the image, the tables
    Json build = Json::MakeArray();
    for (const Result& r : res) {
        Json e = Json::MakeObject();
        e.set("key", r.key);
        e.set("st", r.st);
        e.set("err", r.err);
        build.push_back(e);
    }
    std::map<std::string, std::string> bytes = {
        {"_b_build.json", JsonLine(build, false)},
        {std::string(kStamp), absl::StrCat("# the game and the profile inputs this library was built from "
                                           "(dm/gfx/tools/dmsh_chain.py inputs)\ngame\t",
                                           game_id_, "\ninputs\t", inputs_, "\n")}};
    std::vector<std::pair<std::string, std::string>> files;
    int ok = 0;
    for (const Result& r : res) {
        if (r.st != "ok") continue;
        ++ok;
        const std::optional<std::string> dir = EntryDir(r.key);
        if (!dir) continue;
        for (const stdfs::directory_entry& e : stdfs::directory_iterator(*dir, ec)) {
            const std::string n = e.path().filename().string();
            if (!py::StartsWith(n, ".") && !py::Contains(n, ".tmp")) files.emplace_back(n, e.path().string());
        }
    }
    Print(absl::StrCat("library stamp: game ", game_id_, ", inputs ", inputs_));
    if (absl::Status s = WriteImage(image_, files, bytes); !s.ok())
        return absl::UnavailableError(absl::StrCat("[RED] could not write ", image_, ": ", s.message()));
    try {
        WriteTables(res);
    } catch (const py::Fatal& e) {
        return absl::FailedPreconditionError(e.what());
    }
    BuildResult result;
    result.shaders = static_cast<int>(res.size());
    result.ok = ok;
    result.image = image_;
    return result;
}

}  // namespace

absl::StatusOr<BuildResult> Build(const BuildOptions& options) {
    Builder b(options);
    return b.Run();
}

}  // namespace nxmac::dmsh
