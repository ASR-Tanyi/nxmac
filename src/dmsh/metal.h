// Apple's Metal compiler as the shader build runs it (internal to nxmac::dmsh): nxnative's dmsh_build.py mac_compile,
// dmsh_pch.py (a precompiled header that changes no output, only the compile's time) and dmsh_mtlcache.py (a library is
// compiled once per Metal source, compile convention and compiler, and kept). The tools are the ones xcrun names (on a
// Mac without Xcode 26 there are none); nxmac compiles for macOS 15 and later, which the convention's record says.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "json.h"

namespace nxmac::dmsh {

class Metal {
public:
    // Finds the tools (xcrun -sdk macosx -f metal / metallib, the SDK path) and the compiler's identity. cache: the
    // folder of compiled libraries; work: a folder for temporary files; min_macos: the libraries' minimum macOS
    // (-mmacosx-version-min; "" compiles as nxnative does, for the SDK's own)
    static absl::StatusOr<std::unique_ptr<Metal>> Open(const std::string& cache, const std::string& work,
                                                       const std::string& min_macos);

    // The library of msl (a file) at lib: the one the same source compiled to before, or a compile, then kept.
    // FailedPrecondition with what Python's CalledProcessError says when the source does not compile
    absl::Status Compile(const std::string& msl, const std::string& lib);
    // The libraries libs cut from msl's one compile by split (the whole library's bytes → each one's bytes), each kept
    using Split = std::function<std::vector<std::string>(const std::string&)>;  // throws py::ValueError
    absl::Status CompileSplit(const std::string& msl, const std::vector<std::string>& libs, const Split& split);

    // refl with what the reflection records of the compile: chain_metal (the convention), chain_compiler (its identity)
    Json WithCompiler(const Json& refl) const;

    // The compile convention, as the reflection records it
    const std::string& record() const { return record_; }
    // metal --version's first line
    const std::string& version() const { return version_; }
    // What the tools and the convention add to every shader's cache key
    std::string Fingerprint() const;
    std::string Summary() const;

private:
    Metal() = default;
    std::string CachePath(const std::string& src) const;
    absl::Status CompileFresh(const std::string& msl, const std::string& lib);
    std::optional<std::string> Header();  // the precompiled header, made at the first compile (none: not used)

    std::string cache_, work_;
    std::vector<std::string> compile_;  // metal -isysroot <sdk> -c ... up to the source
    std::string metallib_;
    std::string record_, compiler_id_, ident_hex_, ident_;
    std::string version_;
    std::mutex header_lock_;
    bool header_made_ = false;
    std::optional<std::string> header_;
    std::atomic<int> reused_{0}, compiled_{0}, tmp_{0};
};

// Writes data to path by way of a temporary file beside it (os.replace)
absl::Status WriteFileAtomic(const std::string& path, std::string_view data);
absl::StatusOr<std::string> ReadFile(const std::string& path);

}  // namespace nxmac::dmsh
