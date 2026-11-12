#include "files.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/strip.h"

namespace nxmac::shader::internal {
namespace {

namespace stdfs = std::filesystem;

// os.walk below root: files, not following links to folders (a link to a file, or a broken one, is listed as a file)
void Walk(const stdfs::path& dir, const std::string& rel, std::vector<RomFsSource::File>& out,
          std::set<std::string>& paths) {
    std::error_code ec;
    stdfs::directory_iterator it(dir, ec);
    if (ec) return;                                     // os.walk skips a folder it cannot list
    std::vector<std::pair<std::string, bool>> entries;  // name, walked into
    for (; it != stdfs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const stdfs::directory_entry& e = *it;
        std::error_code e1, e2;
        const bool is_dir = e.is_directory(e1);
        const bool is_link = e.is_symlink(e2);
        const std::string name = e.path().filename().string();
        if (is_dir) {
            paths.insert(rel.empty() ? name : absl::StrCat(rel, "/", name));
            if (!is_link) entries.emplace_back(name, true);
        } else {
            entries.emplace_back(name, false);
        }
    }
    for (const auto& [name, folder] : entries) {
        const std::string sub = rel.empty() ? name : absl::StrCat(rel, "/", name);
        if (folder) {
            Walk(dir / name, sub, out, paths);
        } else {
            out.push_back({sub, 0});
            paths.insert(sub);
        }
    }
}

}  // namespace

bool IsImage(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[8] = {};
    if (!in || !in.read(magic, 8)) return false;
    return std::memcmp(magic, "DMROMFS1", 8) == 0;
}

absl::StatusOr<std::unique_ptr<RomFsSource>> RomFsSource::Open(const std::string& path) {
    auto src = std::unique_ptr<RomFsSource>(new RomFsSource());
    src->root_ = path;
    std::error_code ec;
    if (stdfs::is_regular_file(path, ec) && IsImage(path)) {
        absl::StatusOr<fs::StoragePtr> storage = fs::OpenFileStorage(path);
        if (!storage.ok()) return storage.status();
        absl::StatusOr<fs::RomFsImage> image = fs::RomFsImage::Open(*std::move(storage));
        if (!image.ok()) return absl::Status(image.status().code(), absl::StrCat(path, ": ", image.status().message()));
        src->image_.emplace(*std::move(image));
        const std::vector<fs::RomFsImageEntry>& entries = src->image_->entries();
        for (std::size_t k = 0; k < entries.size(); ++k) {
            const fs::RomFsImageEntry& e = entries[k];
            if (e.path.empty()) continue;  // the root
            const std::string rel(absl::StripPrefix(e.path, "/"));
            src->paths_.insert(rel);
            if (!e.directory) src->files_.push_back({rel, k});
        }
    } else if (stdfs::is_directory(path, ec)) {
        Walk(stdfs::path(path), "", src->files_, src->paths_);
    } else {
        return absl::NotFoundError(absl::StrFormat("%s: no RomFS folder or DMROMFS1 image there", path));
    }
    // sorted((path, rel)): every path is the root's followed by rel, so rel's bytes give the order
    std::sort(src->files_.begin(), src->files_.end(), [](const File& a, const File& b) { return a.rel < b.rel; });
    return src;
}

absl::StatusOr<std::string> RomFsSource::Read(const File& file, std::uint64_t n) const {
    if (image_) {
        const fs::RomFsImageEntry& e = image_->entries()[file.entry];
        absl::StatusOr<fs::StoragePtr> storage = image_->OpenFile(e);
        if (!storage.ok()) return storage.status();
        const std::uint64_t size = n ? std::min<std::uint64_t>(n, (*storage)->size()) : (*storage)->size();
        std::string out(size, '\0');
        absl::Status read = (*storage)->Read(0, std::span(reinterpret_cast<std::uint8_t*>(out.data()), out.size()));
        if (!read.ok()) return read;
        return out;
    }
    const stdfs::path p = stdfs::path(root_) / file.rel;
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return absl::NotFoundError(absl::StrFormat("[Errno %d] %s: '%s'", errno, std::strerror(errno), p.string()));
    std::string out;
    if (n) {
        out.resize(n);
        in.read(out.data(), static_cast<std::streamsize>(n));
        out.resize(static_cast<std::size_t>(in.gcount()));
        return out;
    }
    std::error_code ec;
    const std::uintmax_t size = stdfs::file_size(p, ec);
    if (!ec) {
        out.resize(static_cast<std::size_t>(size));
        in.read(out.data(), static_cast<std::streamsize>(size));
        out.resize(static_cast<std::size_t>(in.gcount()));
    }
    char buffer[1 << 16];  // what the size did not count
    while (in.read(buffer, sizeof buffer) || in.gcount()) out.append(buffer, static_cast<std::size_t>(in.gcount()));
    if (in.bad()) return absl::DataLossError(absl::StrFormat("%s: a read failed", p.string()));
    return out;
}

std::uint64_t RomFsSource::Size(const File& file) const {
    if (image_) return image_->entries()[file.entry].b;
    std::error_code ec;
    const std::uintmax_t size = stdfs::file_size(stdfs::path(root_) / file.rel, ec);
    return ec ? 0 : size;
}

absl::StatusOr<std::string> RomFsSource::ReadAt(const File& file, std::uint64_t offset, std::uint64_t n) const {
    const std::uint64_t size = Size(file);
    if (offset >= size) return std::string();
    n = std::min(n, size - offset);
    std::string out(n, '\0');
    if (image_) {
        absl::StatusOr<fs::StoragePtr> storage = image_->OpenFile(image_->entries()[file.entry]);
        if (!storage.ok()) return storage.status();
        absl::Status read =
            (*storage)->Read(offset, std::span(reinterpret_cast<std::uint8_t*>(out.data()), out.size()));
        if (!read.ok()) return read;
        return out;
    }
    std::ifstream in(stdfs::path(root_) / file.rel, std::ios::binary);
    if (!in) return absl::NotFoundError(absl::StrFormat("%s: %s", file.rel, std::strerror(errno)));
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(out.data(), static_cast<std::streamsize>(n));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return out;
}

bool RomFsSource::Exists(const std::string& rel) const {
    if (image_) return paths_.count(rel) > 0;
    std::error_code ec;
    return stdfs::exists(stdfs::path(root_) / rel, ec);
}

}  // namespace nxmac::shader::internal
