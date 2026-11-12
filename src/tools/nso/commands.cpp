#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string_view>
#include <thread>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "nso/nso.h"
#include "nso/options.h"
#include "nxmac/rewriter/module.h"
#include "nxmac/rewriter/patches.h"
#include "nxmac/rewriter/rewrite.h"

namespace nxmac::nso {
namespace {

namespace fsys = std::filesystem;

std::string ModuleName(const Options& options, const std::string& path) {
    return options.name.empty() ? fsys::path(path).filename().string() : options.name;
}

absl::Status WriteFile(const fsys::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !out.flush()) {
        return absl::UnknownError(absl::StrFormat("%s: %s", path.string(), std::strerror(errno)));
    }
    return absl::OkStatus();
}

struct Outcome {
    absl::Status status;
    std::vector<std::string> lines;  // the rewrite's log, then its summary
};

// One module: read, rewritten, and its files written once the whole rewrite has succeeded
Outcome RewriteOne(const Options& options, const rewriter::PatchTables* tables, const std::string& path) {
    Outcome outcome;
    const std::string name = ModuleName(options, path);
    absl::StatusOr<rewriter::Module> module = rewriter::ReadModule(path);
    if (!module.ok()) {
        outcome.status = module.status();
        return outcome;
    }
    rewriter::RewriteOptions rewrite;
    rewrite.name = name;
    rewrite.game = options.game;
    rewrite.tables = tables;
    rewrite.list_spin_waits = options.spin_list;
    absl::StatusOr<rewriter::RewriteResult> result = rewriter::Rewrite(*module, rewrite);
    if (!result.ok()) {
        outcome.status = result.status();
        return outcome;
    }
    outcome.lines = std::move(result->log);
    outcome.lines.push_back(result->summary);
    const fsys::path dir(options.out);
    const auto file = [&](std::string_view ext) { return dir / absl::StrCat(name, ".", ext); };
    const std::string_view patched(reinterpret_cast<const char*>(result->patched.data()),  // NOLINT: bytes as chars
                                   result->patched.size());
    const std::pair<std::string_view, std::string_view> files[] = {
        {"patched.bin", patched},     {"fixups.tsv", result->fixups}, {"gates.tsv", result->gates},
        {"sites.tsv", result->sites}, {"cuts.tsv", result->cuts},     {"changed.tsv", result->changed},
        {"obs.tsv", result->obs}};
    for (const auto& [ext, bytes] : files) {
        if (absl::Status s = WriteFile(file(ext), bytes); !s.ok()) {
            outcome.status = std::move(s);
            return outcome;
        }
    }
    // A registered build has no match table: one left by an earlier rewrite of another build would mislead
    std::error_code ignored;
    if (result->match) {
        outcome.status = WriteFile(file("match.tsv"), *result->match);
    } else {
        fsys::remove(file("match.tsv"), ignored);
    }
    return outcome;
}

int Rewrite(const Options& options, const rewriter::PatchTables* tables, std::ostream& out, std::ostream& err) {
    std::set<std::string> names;
    for (const std::string& path : options.modules) {
        if (!names.insert(ModuleName(options, path)).second) {
            err << "nso: two modules are named " << ModuleName(options, path)
                << ": their files would overwrite each other\n";
            return kUsage;
        }
    }
    std::error_code ec;
    fsys::create_directories(options.out, ec);
    if (ec) {
        err << "nso: " << options.out << ": " << ec.message() << "\n";
        return kFailed;
    }
    // Each module on a thread of its own; their reports in the order of the command line
    std::vector<Outcome> outcomes(options.modules.size());
    std::atomic<std::size_t> next{0};
    const std::size_t cpus = std::max(1U, std::thread::hardware_concurrency());
    const std::size_t jobs =
        std::min(options.modules.size(), options.jobs != 0 ? static_cast<std::size_t>(options.jobs) : cpus);
    const auto work = [&] {
        for (std::size_t i = next++; i < options.modules.size(); i = next++) {
            outcomes[i] = RewriteOne(options, tables, options.modules[i]);
        }
    };
    std::vector<std::thread> threads;
    for (std::size_t i = 1; i < jobs; ++i) threads.emplace_back(work);
    work();
    for (std::thread& t : threads) t.join();
    int code = kOk;
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        const Outcome& o = outcomes[i];
        for (const std::string& line : o.lines) out << line << "\n";
        if (!o.status.ok()) {
            err << "[RED] " << options.modules[i] << ": " << o.status.message() << "\n";
            code = kFailed;
        }
    }
    return code;
}

int Info(const Options& options, const rewriter::PatchTables& tables, std::ostream& out, std::ostream& err) {
    int code = kOk;
    static constexpr const char* kSegments[] = {"text", "ro", "data"};
    for (const std::string& path : options.modules) {
        const absl::StatusOr<rewriter::Module> module = rewriter::ReadModule(path);
        if (!module.ok()) {
            err << "[RED] " << module.status().message() << "\n";
            code = kFailed;
            continue;
        }
        const std::string build = module->BuildId();
        out << path << "\n  build ID " << build << "\n";
        for (std::size_t i = 0; i < 3; ++i) {
            out << absl::StrFormat("  %-4s at 0x%x, 0x%x bytes\n", kSegments[i], module->segments[i].offset,
                                   module->segments[i].size);
        }
        out << absl::StrFormat("  bss  0x%x bytes\n", module->bss);
        const auto* cuts = tables.Cuts(build);
        const auto* obs = tables.Obs(build);
        if (cuts == nullptr && obs == nullptr && tables.FrameStep(build) == nullptr && tables.Stubs(build) == nullptr) {
            out << "  patch tables: the build is not registered (function-matched patches may still apply)\n";
            continue;
        }
        out << absl::StrFormat("  patch tables: %d cuts, %d observation gates", cuts != nullptr ? cuts->size() : 0,
                               obs != nullptr ? obs->size() : 0);
        if (const std::string* profile = tables.FrameStep(build))
            out << ", frame-step stubs (profile " << *profile << ")";
        if (const std::string* profile = tables.Stubs(build)) out << ", stubs (profile " << *profile << ")";
        if (const auto* capabilities = tables.Capabilities(build)) {
            for (const std::string& c : *capabilities) out << ", " << c;
        }
        out << "\n";
    }
    return code;
}

}  // namespace

int RunNso(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const absl::StatusOr<Options> options = ParseOptions(args);
    if (!options.ok()) {
        err << "nso: " << options.status().message() << "\n\n" << Usage();
        return kUsage;
    }
    if (options->command == "help") {
        out << Usage();
        return kOk;
    }
    const rewriter::PatchTables* tables = &rewriter::PatchTables::BuiltIn();
    absl::StatusOr<rewriter::PatchTables> given = rewriter::PatchTables();
    if (!options->patches.empty()) {
        given = rewriter::PatchTables::ReadFile(options->patches);
        if (!given.ok()) {
            err << "nso: " << given.status().message() << "\n";
            return kFailed;
        }
        tables = &*given;
    }
    return options->command == "info" ? Info(*options, *tables, out, err) : Rewrite(*options, tables, out, err);
}

}  // namespace nxmac::nso
