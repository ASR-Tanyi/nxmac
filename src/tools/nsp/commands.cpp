#include "nsp/commands.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "nsp/nsp.h"

namespace nxmac::nsp {

bool OpenedPackage::Holds(fs::ContentMetaType type) const {
    return std::any_of(metas.begin(), metas.end(), [&](const fs::ContentMeta& m) { return m.type == type; });
}

absl::StatusOr<OpenedPackage> OpenPackage(const fs::KeySet& keys, const std::string& path) {
    absl::StatusOr<fs::Package> package = fs::Package::OpenFile(keys, path);
    if (!package.ok()) return package.status();
    absl::StatusOr<std::vector<fs::ContentMeta>> metas = package->ContentMetas();
    if (!metas.ok()) {
        return absl::Status(metas.status().code(),
                            absl::StrCat(path, ": its content metas: ", metas.status().message()));
    }
    return OpenedPackage{path, std::filesystem::path(path).filename().string(), *std::move(package), *std::move(metas)};
}

std::string Size(std::uint64_t bytes) {
    if (bytes < 1000) return absl::StrFormat("%u bytes", bytes);
    if (bytes < 1000000) return absl::StrFormat("%.1f KB", static_cast<double>(bytes) / 1e3);
    if (bytes < 1000000000) return absl::StrFormat("%.1f MB", static_cast<double>(bytes) / 1e6);
    return absl::StrFormat("%.2f GB", static_cast<double>(bytes) / 1e9);
}

std::string_view ContentTypeName(fs::NcaContentType type) {
    switch (type) {
        case fs::NcaContentType::kProgram:
            return "program";
        case fs::NcaContentType::kMeta:
            return "meta";
        case fs::NcaContentType::kControl:
            return "control";
        case fs::NcaContentType::kManual:
            return "manual";
        case fs::NcaContentType::kData:
            return "data";
        case fs::NcaContentType::kPublicData:
            return "public data";
    }
    return "?";
}

std::string_view MetaTypeName(fs::ContentMetaType type) {
    switch (type) {
        case fs::ContentMetaType::kApplication:
            return "game";
        case fs::ContentMetaType::kPatch:
            return "update";
        case fs::ContentMetaType::kAddOnContent:
            return "add-on content";
        case fs::ContentMetaType::kDataPatch:
            return "add-on content update";
        case fs::ContentMetaType::kDelta:
            return "delta";
        default:
            return "system content";
    }
}

bool KeysOpen(const Context& ctx, const std::vector<NamedNca>& ncas) {
    bool open = true;
    for (const NamedNca& n : ncas) {
        if (n.nca == nullptr || n.nca->key_status().ok()) continue;
        ctx.err << "[keys] " << n.file << ": " << n.nca->key_status().message() << "\n";
        open = false;
    }
    return open;
}

int Copy(const Context& ctx, const std::vector<Item>& items, CopyResult* result) {
    const auto start = std::chrono::steady_clock::now();
    Progress progress;
    if (ctx.progress) {
        progress = [&ctx, start](std::uint64_t done, std::uint64_t total) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            ctx.err << absl::StrFormat("\r  %s of %s, %.0f MB/s   ", Size(done), Size(total),
                                       static_cast<double>(done) / 1e6 / std::max(seconds, 1e-3))
                    << std::flush;
        };
    }
    *result = CopyItems(items, ctx.jobs, progress);
    if (ctx.progress) ctx.err << "\n";
    int code = kOk;
    for (const Failure& f : result->failures) {
        // A file that did not come out whole is not left behind (an image's .part file is its caller's)
        if (items[f.item].destination.kind == Destination::Kind::kFile) {
            std::remove(items[f.item].destination.path.c_str());
        }
        const bool damaged = f.status.code() == absl::StatusCode::kDataLoss;
        ctx.err << (damaged ? "[damaged] " : "[error] ") << items[f.item].name << ": " << f.status.message() << "\n";
        code = std::max(code, damaged ? static_cast<int>(kDamaged) : static_cast<int>(kFailed));
    }
    if (code == kDamaged) {
        ctx.err << absl::StrFormat(
            "[damaged] %u files fail the NCAs' hash checks: an incomplete copy or a broken download; copy or download "
            "the package again\n",
            result->failures.size());
    }
    return code == kDamaged || code == kOk ? code : kFailed;
}

bool SafeName(std::string_view name) {
    return !name.empty() && name != "." && name != ".." && !absl::StrContains(name, '/') &&
           !absl::StrContains(name, '\0');
}

absl::Status CreateFile(const std::string& path, std::uint64_t size) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec) return absl::InternalError(absl::StrCat("creating the folder of ", path, ": ", ec.message()));
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return absl::ErrnoToStatus(errno, absl::StrCat("creating ", path));
    const int rc = ::ftruncate(fd, static_cast<off_t>(size));
    const int err = errno;
    ::close(fd);
    if (rc != 0) return absl::ErrnoToStatus(err, absl::StrCat("sizing ", path));
    return absl::OkStatus();
}

int RunNsp(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, bool progress) {
    absl::StatusOr<Options> options = ParseOptions(args);
    if (!options.ok()) {
        err << "nsp: " << options.status().message() << "\n(nsp help lists the commands and options)\n";
        return kUsage;
    }
    if (options->command == "help") {
        out << Usage();
        return kOk;
    }
    std::string keys_path = options->keys;
    if (keys_path.empty()) {
        const char* home = std::getenv("HOME");
        keys_path = absl::StrCat(home != nullptr ? home : "", "/.switch/prod.keys");
    }
    absl::StatusOr<fs::KeySet> keys = fs::KeySet::Load(keys_path);
    if (!keys.ok()) {
        err << "nsp: " << keys.status().message() << (options->keys.empty() ? " (give your prod.keys with -k)" : "")
            << "\n";
        return kFailed;
    }
    const int jobs =
        options->jobs > 0 ? options->jobs : static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 64u));
    const Context ctx{*options, *keys, out, err, progress, jobs};
    if (options->command == "info") return Info(ctx);
    if (options->command == "verify") return Verify(ctx);
    if (options->command == "extract") return Extract(ctx);
    return Dlc(ctx);
}

}  // namespace nxmac::nsp
