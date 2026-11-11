// nsp's commands and what they share (internal to the tool).
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "nsp/copy.h"
#include "nsp/options.h"
#include "nxmac/fs/content_meta.h"
#include "nxmac/fs/keys.h"
#include "nxmac/fs/nca.h"
#include "nxmac/fs/package.h"

namespace nxmac::nsp {

struct Context {
    const Options& options;
    const fs::KeySet& keys;
    std::ostream& out;
    std::ostream& err;
    bool progress = false;
    int jobs = 1;
};

// A package opened with its content metas read
struct OpenedPackage {
    std::string path;
    std::string name;  // its file name, for messages
    fs::Package package;
    std::vector<fs::ContentMeta> metas;

    bool Holds(fs::ContentMetaType type) const;
};
absl::StatusOr<OpenedPackage> OpenPackage(const fs::KeySet& keys, const std::string& path);

int Info(const Context& ctx);
int Verify(const Context& ctx);
int Extract(const Context& ctx);
int Dlc(const Context& ctx);

// ---- shared ----
std::string Size(std::uint64_t bytes);  // "512 bytes", "2.4 KB", "12.3 MB", "13.40 GB"
std::string_view ContentTypeName(fs::NcaContentType type);
std::string_view MetaTypeName(fs::ContentMetaType type);

struct NamedNca {
    std::string file;  // the package's file name, for messages
    const fs::Nca* nca;
};
// Prints a line for each NCA whose content key is missing; false when there is one
bool KeysOpen(const Context& ctx, const std::vector<NamedNca>& ncas);

// Runs the copy with a progress line on a terminal, prints a line per failed item and returns the exit code they make
// (kOk, kDamaged for hash checks, kFailed for anything else)
int Copy(const Context& ctx, const std::vector<Item>& items, CopyResult* result);

// A name from the game that becomes a file's name: not empty, ".", ".." or holding a "/" or NUL
bool SafeName(std::string_view name);
// Creates (or empties) a file of size bytes, with its directories
absl::Status CreateFile(const std::string& path, std::uint64_t size);

}  // namespace nxmac::nsp
