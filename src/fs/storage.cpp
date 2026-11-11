#include "nxmac/fs/storage.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace nxmac::fs {
namespace {

class FileStorage final : public Storage {
public:
    FileStorage(int fd, std::uint64_t size, std::string path) : fd_(fd), size_(size), path_(std::move(path)) {}
    ~FileStorage() override { ::close(fd_); }

    absl::Status Read(std::uint64_t offset, std::span<std::uint8_t> out) const override {
        if (absl::Status s = CheckRange(offset, out.size(), size_); !s.ok()) return s;
        // pread takes at most INT_MAX bytes at a time on macOS
        constexpr std::size_t kMaxChunk = std::size_t{1} << 30;
        std::size_t done = 0;
        while (done < out.size()) {
            const std::size_t want = std::min(out.size() - done, kMaxChunk);
            const ssize_t got = ::pread(fd_, out.data() + done, want, static_cast<off_t>(offset + done));
            if (got < 0) {
                if (errno == EINTR) continue;
                return absl::ErrnoToStatus(errno, absl::StrFormat("%s: read at %#x", path_, offset + done));
            }
            if (got == 0) {
                return absl::DataLossError(absl::StrFormat("%s: ends at %#x while reading", path_, offset + done));
            }
            done += static_cast<std::size_t>(got);
        }
        return absl::OkStatus();
    }

    std::uint64_t size() const override { return size_; }

private:
    int fd_;
    std::uint64_t size_;
    std::string path_;
};

class MemoryStorage final : public Storage {
public:
    explicit MemoryStorage(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    absl::Status Read(std::uint64_t offset, std::span<std::uint8_t> out) const override {
        if (absl::Status s = CheckRange(offset, out.size(), bytes_.size()); !s.ok()) return s;
        if (!out.empty()) std::memcpy(out.data(), bytes_.data() + offset, out.size());
        return absl::OkStatus();
    }

    std::uint64_t size() const override { return bytes_.size(); }

private:
    std::vector<std::uint8_t> bytes_;
};

class SubStorage final : public Storage {
public:
    SubStorage(StoragePtr base, std::uint64_t offset, std::uint64_t size)
        : base_(std::move(base)), offset_(offset), size_(size) {}

    absl::Status Read(std::uint64_t offset, std::span<std::uint8_t> out) const override {
        if (absl::Status s = CheckRange(offset, out.size(), size_); !s.ok()) return s;
        return base_->Read(offset_ + offset, out);
    }

    std::uint64_t size() const override { return size_; }

private:
    StoragePtr base_;
    std::uint64_t offset_;
    std::uint64_t size_;
};

}  // namespace

absl::Status CheckRange(std::uint64_t offset, std::uint64_t size, std::uint64_t total) {
    if (offset > total || size > total - offset) {
        return absl::OutOfRangeError(
            absl::StrFormat("bytes %#x-%#x are past the end of a storage of %#x bytes", offset, offset + size, total));
    }
    return absl::OkStatus();
}

absl::StatusOr<StoragePtr> OpenFileStorage(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return absl::ErrnoToStatus(errno, absl::StrCat("cannot open ", path));
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        ::close(fd);
        return absl::ErrnoToStatus(err, absl::StrCat("cannot stat ", path));
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        return absl::InvalidArgumentError(absl::StrCat(path, " is not a regular file"));
    }
    return std::make_shared<FileStorage>(fd, static_cast<std::uint64_t>(st.st_size), path);
}

StoragePtr MakeMemoryStorage(std::vector<std::uint8_t> bytes) {
    return std::make_shared<MemoryStorage>(std::move(bytes));
}

absl::StatusOr<StoragePtr> MakeSubStorage(StoragePtr base, std::uint64_t offset, std::uint64_t size) {
    if (absl::Status s = CheckRange(offset, size, base->size()); !s.ok()) return s;
    return std::make_shared<SubStorage>(std::move(base), offset, size);
}

absl::StatusOr<std::vector<std::uint8_t>> ReadBytes(const Storage& storage, std::uint64_t offset, std::uint64_t size) {
    if (absl::Status s = CheckRange(offset, size, storage.size()); !s.ok()) return s;
    std::vector<std::uint8_t> bytes(size);
    if (absl::Status s = storage.Read(offset, bytes); !s.ok()) return s;
    return bytes;
}

}  // namespace nxmac::fs
