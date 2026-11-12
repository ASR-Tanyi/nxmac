// The split's scan of one file and of the bytes in it (internal to nxmac::shader): shader_split.py's scan_file,
// scan_blob and uniq_keys, with what nxmac has not ported reported as such.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "files.h"
#include "nvn.h"
#include "readers.h"

namespace nxmac::shader::internal {

// What archives in archives may still open (scan_blob's left: what members add up to, what Yaz0 expands to)
struct Budget {
    PyInt members = 0;
    PyInt expanded = 0;
};

void ScanBlob(Context& cx, Bytes d, const std::string& label, Found& out, int depth = 0, Budget* left = nullptr);

// One shader per key, the first found (red() when a key's bytes differ)
using Keyed = std::vector<std::pair<std::string, Shader>>;
Keyed UniqKeys(const Found& found, const KeySkip& skip);

struct FileResult {
    std::string rel;
    Keyed uniq;
    std::set<VarPair> pairs;
    std::optional<std::string> unreadable;  // why the file was left out
};

// scan_file: a file of a RomFS, rel its name in the tables (a later version's and DLC's prefixed)
FileResult ScanFile(const RomFsSource& src, const RomFsSource::File& file, const std::string& rel, const KeySkip& skip,
                    const std::function<void(const std::string&)>& print);

// Why a RomFS file belongs to an archive or engine whose reader nxmac has not ported (shader_split.py takes such
// files apart in jobs of their own before the scan), or nullopt
std::optional<std::string> UnportedArchive(const RomFsSource& src, const RomFsSource::File& file);

}  // namespace nxmac::shader::internal
