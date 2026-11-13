#include "metal.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <thread>

#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "nxmac/fs/crypto.h"
#include "process.h"
#include "py.h"
#include "re.h"

namespace nxmac::dmsh {
namespace {

namespace stdfs = std::filesystem;

// What changes a compiled library: the convention (dmsh_build.py MAC_METAL, with nxmac's minimum macOS), and the
// bracket depth and precompiled header, which change no output (so the record does not name them)
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const std::vector<std::string> kConvention = {"-c", "-O1", "-fpreserve-invariance", "-fwrapv"};
const char* const kDepth = "-fbracket-depth=1024";
constexpr std::string_view kFormat =
    "dmsh_mtlcache 2";  // dmsh_mtlcache.FORMAT: the identity is nxnative's for the same convention

std::string Sha256(std::string_view data) {
    const fs::Sha256Hash h = fs::Sha256(std::span(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
    return std::string(reinterpret_cast<const char*>(h.data()), h.size());
}

std::string Hex(std::string_view bytes) { return absl::BytesToHexString(bytes); }

absl::StatusOr<std::string> Xcrun(const std::vector<std::string>& args) {
    std::vector<std::string> argv = {"xcrun", "-sdk", "macosx"};
    argv.insert(argv.end(), args.begin(), args.end());
    ProcessResult r = RunProcess(argv);
    if (r.code != 0) {
        return absl::FailedPreconditionError(absl::StrCat(
            "xcrun ", absl::StrJoin(args, " "), " failed", r.error.empty() ? "" : ": " + r.error,
            r.err.empty() ? "" : ": " + std::string(py::Strip(r.err)),
            " (the Metal compiler comes with Xcode 26 or later, xcode-select -s /Applications/Xcode.app; when Xcode "
            "says its Metal Toolchain is missing: xcodebuild -downloadComponent MetalToolchain)"));
    }
    return r.out;
}

// The precompiled header's source and the shader it is checked on (dmsh_pch.py HEADER, PROBE)
std::string PchHeader() {
    const char* sampled[] = {"texture2d", "texture2d_array", "texture3d", "texturecube",    "texturecube_array",
                             "depth2d",   "depth2d_array",   "depthcube", "depthcube_array"};
    const char* written[] = {"texture2d", "texture2d_array", "texture3d"};
    std::string h = "#include <metal_stdlib>\n#include <simd/simd.h>\n";
    for (const char* k : sampled)
        for (const char* c : {"threadgroup", "device"})
            absl::StrAppend(&h, "static_assert(sizeof(metal::", k,
                            "<float, metal::access::sample, metal::memory_coherence_", c, ">) > 0);\n");
    for (const char* k : written)
        absl::StrAppend(&h, "static_assert(sizeof(metal::", k,
                        "<float, metal::access::write, metal::memory_coherence_threadgroup>) > 0);\n");
    return h;
}

const char* const kProbe = R"(#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;
struct set0 { texture2d<float> a [[id(0)]]; texture2d_array<float> b [[id(1)]]; texture3d<float> c [[id(2)]]; texturecube<float> d [[id(3)]]; depth2d_array<float> e [[id(4)]]; sampler s [[id(5)]]; };
struct main0_in { float4 uv [[user(locn0)]]; };
fragment float4 main0(main0_in in [[stage_in]], constant set0& t [[buffer(0)]])
{
    return t.a.sample(t.s, in.uv.xy) + t.b.sample(t.s, in.uv.xy, 0u) + t.c.sample(t.s, in.uv.xyz) + t.d.sample(t.s, in.uv.xyz) + t.e.sample(t.s, in.uv.xy, 0u);
}
)";

std::string TempName(const std::string& path, std::atomic<int>& n) {
    return absl::StrCat(path, ".tmp-", getpid(), "-", n++);
}

}  // namespace

absl::Status WriteFileAtomic(const std::string& path, std::string_view data) {
    static std::atomic<int> n{0};
    const std::string tmp = TempName(path, n);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return absl::UnavailableError(absl::StrCat(tmp, ": cannot be written"));
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return absl::UnavailableError(absl::StrCat(tmp, ": write failed (is the disk full?)"));
    }
    std::error_code ec;
    stdfs::rename(tmp, path, ec);
    if (ec) return absl::UnavailableError(absl::StrCat(path, ": ", ec.message()));
    return absl::OkStatus();
}

absl::StatusOr<std::string> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return absl::NotFoundError(absl::StrCat(path, ": cannot be read"));
    std::string out((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return out;
}

absl::StatusOr<std::unique_ptr<Metal>> Metal::Open(const std::string& cache, const std::string& work,
                                                   const std::string& min_macos) {
    auto m = std::unique_ptr<Metal>(new Metal());
    m->cache_ = cache;
    m->work_ = work;
    absl::StatusOr<std::string> metal = Xcrun({"-f", "metal"});
    if (!metal.ok()) return metal.status();
    absl::StatusOr<std::string> metallib = Xcrun({"-f", "metallib"});
    if (!metallib.ok()) return metallib.status();
    absl::StatusOr<std::string> sdk = Xcrun({"--show-sdk-path"});
    if (!sdk.ok()) return sdk.status();
    std::vector<std::string> convention = kConvention;
    if (!min_macos.empty()) convention.push_back(absl::StrCat("-mmacosx-version-min=", min_macos));
    m->compile_ = {std::string(py::Strip(*metal)), "-isysroot", std::string(py::Strip(*sdk))};
    m->compile_.insert(m->compile_.end(), convention.begin(), convention.end());
    m->metallib_ = std::string(py::Strip(*metallib));
    m->record_ =
        absl::StrCat("xcrun -sdk macosx metal ", absl::StrJoin(convention, " "), "; xcrun -sdk macosx metallib");
    // dmsh_mtlcache.compiler_id: the tools' versions without their install path, the SDK's version and build
    std::string tools;
    for (const char* t : {"metal", "metallib"}) {
        absl::StatusOr<std::string> v = Xcrun({t, "--version"});
        if (!v.ok()) return v.status();
        tools += *v;
        if (std::string_view(t) == "metal") m->version_ = py::Split(*v, "\n")[0];
    }
    static const re::Pattern installed(R"(^InstalledDir:.*\n?)", re::M);
    std::string id = installed.sub("", tools);
    for (const char* q : {"--show-sdk-version", "--show-sdk-build-version"}) {
        absl::StatusOr<std::string> v = Xcrun({q});
        if (!v.ok()) return v.status();
        id += *v;
    }
    m->compiler_id_ = id;
    m->ident_ = Sha256(absl::StrCat(kFormat, std::string_view("\0", 1), m->record_, std::string_view("\0", 1), id));
    m->ident_hex_ = Hex(m->ident_);
    std::error_code ec;
    stdfs::create_directories(stdfs::path(cache) / "metal", ec);
    if (ec) return absl::UnavailableError(absl::StrCat(cache, ": ", ec.message()));
    return m;
}

std::string Metal::Fingerprint() const {
    return absl::StrCat(std::string_view("metal\0", 6), record_, std::string_view("\0", 1), compiler_id_);
}

Json Metal::WithCompiler(const Json& refl) const {
    Json out = refl;
    out.set("chain_metal", record_);
    out.set("chain_compiler", ident_hex_);
    return out;
}

std::string Metal::CachePath(const std::string& src) const {
    const std::string d = Hex(Sha256(absl::StrCat(ident_, std::string_view("\0", 1), src)));
    return (stdfs::path(cache_) / "metal" / d.substr(0, 2) / (d + ".metallib")).string();
}

std::optional<std::string> Metal::Header() {
    if (const char* off = std::getenv("DMSH_PCH"); off && std::string_view(off) == "0") return std::nullopt;
    std::lock_guard<std::mutex> lock(header_lock_);
    if (header_made_) return header_;
    header_made_ = true;
    const stdfs::path d = stdfs::path(work_) / "pch";
    std::error_code ec;
    stdfs::create_directories(d, ec);
    const std::string h = (d / "prefix.h").string(), pch = (d / "prefix.pch").string(),
                      src = (d / "probe.metal").string();
    if (!WriteFileAtomic(h, PchHeader()).ok() || !WriteFileAtomic(src, kProbe).ok()) return std::nullopt;
    auto note = [&](const std::string& why) {
        std::fprintf(stdout, "note: no precompiled Metal header for %s, so each compile takes longer: %s\n",
                     absl::StrJoin(compile_, " ").c_str(), why.c_str());
        std::fflush(stdout);
    };
    std::vector<std::string> make;
    for (const std::string& a : compile_)
        if (a != "-c") make.push_back(a);
    make.push_back(kDepth);
    make.insert(make.end(), {"-x", "metal-header", h, "-o", pch});
    ProcessResult r = RunProcess(make);
    if (r.code != 0) {
        note(absl::StrCat("it does not build: ", py::To(py::Strip(r.err), 200)));
        return std::nullopt;
    }
    std::string outs[2];
    for (int k = 0; k < 2; ++k) {
        std::vector<std::string> argv = compile_;
        argv.push_back(kDepth);
        if (k == 1) argv.insert(argv.end(), {"-include-pch", pch});
        const std::string out = (d / (k ? "header.air" : "plain.air")).string();
        argv.insert(argv.end(), {src, "-o", out});
        ProcessResult c = RunProcess(argv);
        if (c.code != 0) {
            note(absl::StrCat("a test shader does not compile: ", py::To(py::Strip(c.err), 200)));
            return std::nullopt;
        }
        absl::StatusOr<std::string> air = ReadFile(out);
        outs[k] = air.ok() ? *air : "";
    }
    if (outs[0] != outs[1]) {
        note("a test shader compiled with it differs");
        return std::nullopt;
    }
    header_ = pch;
    return header_;
}

absl::Status Metal::CompileFresh(const std::string& msl, const std::string& lib) {
    const std::string air =
        (stdfs::path(work_) / absl::StrCat("mtlcache-mac-", getpid(), "-", tmp_++, ".air")).string();
    struct Remove {
        std::string path;
        ~Remove() {
            std::error_code ec;
            stdfs::remove(path, ec);
        }
    } remove_air{air};
    std::vector<std::string> plain = compile_;
    plain.push_back(kDepth);
    bool done = false;
    if (std::optional<std::string> pch = Header()) {
        std::vector<std::string> argv = plain;
        argv.insert(argv.end(), {"-include-pch", *pch, "-x", "metal", "-", "-o", air});
        ProcessResult r = RunProcess(argv, msl);
        if (r.code == 0) {
            if (!r.err.empty()) std::fwrite(r.err.data(), 1, r.err.size(), stderr);
            done = true;
        }
    }
    if (!done) {
        std::vector<std::string> argv = plain;
        argv.insert(argv.end(), {"-x", "metal", "-", "-o", air});
        ProcessResult r = RunProcess(argv, msl);
        if (!r.err.empty()) std::fwrite(r.err.data(), 1, r.err.size(), stderr);
        if (r.code != 0) {
            std::fprintf(stderr, "%s: does not compile (the messages above say <stdin>)\n", msl.c_str());
            // what Python's CalledProcessError says of the plain compile (dmsh_build.py MAC_RUN + METAL_DEPTH)
            std::vector<std::string> shown = compile_;
            shown.push_back(kDepth);
            shown.insert(shown.end(), {"-x", "metal", "-", "-o", air});
            return absl::FailedPreconditionError(CalledProcessError(shown, r.code));
        }
    }
    ProcessResult l = RunProcess({metallib_, air, "-o", lib});
    if (l.code != 0) {
        if (!l.err.empty()) std::fwrite(l.err.data(), 1, l.err.size(), stderr);
        return absl::FailedPreconditionError(CalledProcessError({metallib_, air, "-o", lib}, l.code));
    }
    return absl::OkStatus();
}

absl::Status Metal::Compile(const std::string& msl, const std::string& lib) {
    absl::StatusOr<std::string> src = ReadFile(msl);
    if (!src.ok()) return src.status();
    const std::string p = CachePath(*src);
    std::error_code ec;
    if (stdfs::exists(p, ec)) {
        stdfs::copy_file(p, lib, stdfs::copy_options::overwrite_existing, ec);
        if (!ec) {
            ++reused_;
            return absl::OkStatus();
        }
    }
    const std::string tmp = absl::StrCat(lib, ".tmp-", getpid(), "-", tmp_++);
    if (absl::Status s = CompileFresh(msl, tmp); !s.ok()) {
        stdfs::remove(tmp, ec);
        return s;
    }
    stdfs::rename(tmp, lib, ec);
    if (ec) return absl::UnavailableError(absl::StrCat(lib, ": ", ec.message()));
    stdfs::create_directories(stdfs::path(p).parent_path(), ec);
    const std::string kept = absl::StrCat(p, ".tmp-", getpid(), "-", tmp_++);
    stdfs::copy_file(lib, kept, stdfs::copy_options::overwrite_existing, ec);
    if (!ec) stdfs::rename(kept, p, ec);
    ++compiled_;
    return absl::OkStatus();
}

absl::Status Metal::CompileSplit(const std::string& msl, const std::vector<std::string>& libs, const Split& split) {
    absl::StatusOr<std::string> src = ReadFile(msl);
    if (!src.ok()) return src.status();
    const std::string p = CachePath(*src);
    std::vector<std::string> paths = {p};
    for (std::size_t i = 1; i < libs.size(); ++i)
        paths.push_back(absl::StrCat(p.substr(0, p.size() - 9), "_x", i > 1 ? absl::StrCat(i) : "", ".metallib"));
    std::error_code ec;
    bool all = true;
    for (const std::string& x : paths) all = all && stdfs::exists(x, ec);
    if (all) {
        bool copied = true;
        for (std::size_t i = 0; i < libs.size(); ++i) {
            stdfs::copy_file(paths[i], libs[i], stdfs::copy_options::overwrite_existing, ec);
            copied = copied && !ec;
        }
        if (copied) {
            ++reused_;
            return absl::OkStatus();
        }
    }
    const std::string whole = absl::StrCat(libs[0], ".tmp-", getpid(), "-", tmp_++);
    if (absl::Status s = CompileFresh(msl, whole); !s.ok()) {
        stdfs::remove(whole, ec);
        return s;
    }
    absl::StatusOr<std::string> bytes = ReadFile(whole);
    stdfs::remove(whole, ec);
    if (!bytes.ok()) return bytes.status();
    std::vector<std::string> pieces;
    try {
        pieces = split(*bytes);
    } catch (const py::ValueError& e) {
        return absl::InvalidArgumentError(e.what());
    }
    if (pieces.size() != libs.size())
        return absl::InvalidArgumentError(
            absl::StrCat("the split gave ", pieces.size(), " libraries, not ", libs.size()));
    for (std::size_t i = 0; i < libs.size(); ++i) {
        if (absl::Status s = WriteFileAtomic(libs[i], pieces[i]); !s.ok()) return s;
    }
    stdfs::create_directories(stdfs::path(p).parent_path(), ec);
    for (std::size_t i = 0; i < libs.size(); ++i) (void)WriteFileAtomic(paths[i], pieces[i]);
    ++compiled_;
    return absl::OkStatus();
}

std::string Metal::Summary() const {
    return absl::StrCat("Metal cache: mac ", reused_.load(), " reused, ", compiled_.load(), " compiled");
}

}  // namespace nxmac::dmsh
