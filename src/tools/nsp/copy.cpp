#include "nsp/copy.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "absl/strings/str_cat.h"

namespace nxmac::nsp {

absl::Status WriteAt(int fd, std::span<const std::uint8_t> bytes, std::uint64_t offset, const std::string& what) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::pwrite(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return absl::ErrnoToStatus(errno, absl::StrCat("writing ", what));
        }
        done += static_cast<std::size_t>(n);
    }
    return absl::OkStatus();
}

CopyResult CopyItems(const std::vector<Item>& items, int threads, const Progress& progress) {
    constexpr std::uint64_t kPiece = 8 << 20;
    struct Piece {
        std::size_t item;
        std::uint64_t offset;
        std::uint64_t size;
    };
    std::vector<Piece> pieces;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const std::uint64_t size = items[i].source->size();
        for (std::uint64_t at = 0; at < size; at += kPiece) pieces.push_back({i, at, std::min(kPiece, size - at)});
        total += size;
    }

    const auto start = std::chrono::steady_clock::now();
    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> done{0};
    const std::unique_ptr<std::atomic<bool>[]> failed(new std::atomic<bool>[items.size()]());
    std::mutex mu;
    std::condition_variable finished;
    int running = 0;
    CopyResult result;

    auto fail = [&](std::size_t item, absl::Status status) {
        if (failed[item].exchange(true)) return;
        const std::lock_guard<std::mutex> lock(mu);
        result.failures.push_back({item, std::move(status)});
    };
    auto work = [&] {
        std::vector<std::uint8_t> buffer(kPiece);
        for (std::size_t p = next++; p < pieces.size(); p = next++) {
            const Piece& piece = pieces[p];
            if (failed[piece.item]) continue;
            const Item& item = items[piece.item];
            const std::span<std::uint8_t> bytes(buffer.data(), piece.size);
            if (absl::Status s = item.source->Read(piece.offset, bytes); !s.ok()) {
                fail(piece.item, std::move(s));
                continue;
            }
            const Destination& to = item.destination;
            if (to.kind == Destination::Kind::kFile) {
                const int fd = ::open(to.path.c_str(), O_WRONLY | O_CLOEXEC);
                if (fd < 0) {
                    fail(piece.item, absl::ErrnoToStatus(errno, absl::StrCat("opening ", to.path)));
                    continue;
                }
                absl::Status s = WriteAt(fd, bytes, piece.offset, to.path);
                ::close(fd);
                if (!s.ok()) {
                    fail(piece.item, std::move(s));
                    continue;
                }
            } else if (to.kind == Destination::Kind::kSharedFile) {
                if (absl::Status s = WriteAt(to.fd, bytes, to.offset + piece.offset, item.name); !s.ok()) {
                    fail(piece.item, std::move(s));
                    continue;
                }
            }
            done += piece.size;
        }
        const std::lock_guard<std::mutex> lock(mu);
        --running;
        finished.notify_all();
    };

    const int n =
        static_cast<int>(std::clamp<std::size_t>(pieces.size(), 1, static_cast<std::size_t>(std::max(threads, 1))));
    std::vector<std::thread> workers;
    workers.reserve(n);
    {
        const std::lock_guard<std::mutex> lock(mu);
        running = n;
    }
    for (int i = 0; i < n; ++i) workers.emplace_back(work);
    {
        std::unique_lock<std::mutex> lock(mu);
        while (running > 0) {
            finished.wait_for(lock, std::chrono::milliseconds(250));
            if (progress && running > 0) {
                lock.unlock();
                progress(done, total);
                lock.lock();
            }
        }
    }
    for (std::thread& t : workers) t.join();
    if (progress) progress(done, total);

    std::sort(result.failures.begin(), result.failures.end(),
              [](const Failure& a, const Failure& b) { return a.item < b.item; });
    result.bytes = done;
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

}  // namespace nxmac::nsp
