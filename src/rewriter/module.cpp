#include "nxmac/rewriter/module.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "absl/strings/escaping.h"
#include "absl/strings/str_format.h"
#include "lz4.h"
#include "nxmac/fs/crypto.h"
#include "text.h"

namespace nxmac::rewriter {
namespace {

using internal::Hex;

constexpr std::uint64_t kPage = 0x1000;
constexpr std::uint64_t kLz4MaxRatio = 255;     // an LZ4 block produces at most 255 bytes per stored byte
constexpr std::uint64_t kSizeMax = 0x7FFFFFFF;  // Horizon reads segment sizes as s32
constexpr std::uint32_t kNsoZstd = 1U << 7;
constexpr std::uint32_t kNroCompressed = 1U << 1;
constexpr const char* kNames[3] = {"text", "ro", "data"};

std::uint32_t U32(std::span<const std::uint8_t> d, std::size_t at) {
    return static_cast<std::uint32_t>(d[at]) | static_cast<std::uint32_t>(d[at + 1]) << 8 |
           static_cast<std::uint32_t>(d[at + 2]) << 16 | static_cast<std::uint32_t>(d[at + 3]) << 24;
}

struct Stored {
    std::uint64_t file_offset = 0;
    std::uint64_t memory_offset = 0;
    std::uint64_t size = 0;
    std::uint64_t stored_size = 0;  // bytes in the file
};

}  // namespace

std::string Module::BuildId() const {
    return absl::BytesToHexString(
        std::string_view(reinterpret_cast<const char*>(build_id.data()), 0x10));  // NOLINT: bytes as chars
}

absl::StatusOr<Module> LoadModule(std::span<const std::uint8_t> d, std::string_view name) {
    Module module;
    std::array<Stored, 3> stored;
    std::uint32_t flags = 0;
    const auto invalid = [&](const std::string& what) { return absl::InvalidArgumentError(what); };
    if (d.size() >= 4 && std::memcmp(d.data(), "NSO0", 4) == 0) {
        if (d.size() < 0x100) {
            return invalid(absl::StrFormat("%s: the NSO header is cut short (%d bytes)", name, d.size()));
        }
        flags = U32(d, 0xc);
        for (std::size_t i = 0; i < 3; ++i) {
            Stored& s = stored[i];
            s.file_offset = U32(d, 0x10 + i * 0x10);
            s.memory_offset = U32(d, 0x14 + i * 0x10);
            s.size = U32(d, 0x18 + i * 0x10);
            s.stored_size = (flags >> i & 1) != 0 ? U32(d, 0x60 + 4 * i) : s.size;
        }
        module.bss = U32(d, 0x3c);
        std::memcpy(module.build_id.data(), d.data() + 0x40, 0x20);
        for (std::size_t i = 0; i < 3; ++i) {
            const Stored& s = stored[i];
            const bool compressed = (flags >> i & 1) != 0;
            const std::string what = absl::StrFormat("%s: %s segment", name, kNames[i]);
            if (s.memory_offset % kPage != 0) {
                return invalid(absl::StrFormat("%s is at %s, not on a page", what, Hex(s.memory_offset)));
            }
            if (s.size > kSizeMax) {
                return invalid(absl::StrFormat("%s claims %s bytes, more than Horizon loads", what, Hex(s.size)));
            }
            if (s.file_offset + s.stored_size > d.size()) {
                return invalid(absl::StrFormat(
                    "%s is stored at %s..%s, past the end of the file (%s bytes): the file is cut short or damaged",
                    what, Hex(s.file_offset), Hex(s.file_offset + s.stored_size), Hex(d.size())));
            }
            if (compressed && s.stored_size > s.size) {
                return invalid(absl::StrFormat("%s is stored compressed in %s bytes, more than its %s", what,
                                               Hex(s.stored_size), Hex(s.size)));
            }
            if (compressed && (flags & kNsoZstd) == 0 && s.size > s.stored_size * kLz4MaxRatio) {
                return invalid(absl::StrFormat("%s claims %s bytes from %s stored, more than LZ4 can produce", what,
                                               Hex(s.size), Hex(s.stored_size)));
            }
        }
        if (stored[0].memory_offset != 0) {
            return invalid(absl::StrFormat("%s: text segment is at %s, not at 0", name, Hex(stored[0].memory_offset)));
        }
        for (std::size_t i = 1; i < 3; ++i) {
            if (stored[i].memory_offset < stored[i - 1].memory_offset + stored[i - 1].size) {
                return invalid(absl::StrFormat("%s: %s segment at %s overlaps the %s segment before it", name,
                                               kNames[i], Hex(stored[i].memory_offset), kNames[i - 1]));
            }
        }
    } else if (d.size() >= 0x14 && std::memcmp(d.data() + 0x10, "NRO0", 4) == 0) {
        // The file is the image itself: each segment stored at its memory offset, uncompressed; +0x18 the module's
        // size, +0x1c flags, +0x20 text, +0x28 ro, +0x30 data as {offset, size}, +0x38 bss size, +0x40 the build ID
        if (d.size() < 0x80) {
            return invalid(absl::StrFormat("%s: the NRO header is cut short (%d bytes)", name, d.size()));
        }
        const std::uint64_t size = U32(d, 0x18);
        if ((U32(d, 0x1c) & kNroCompressed) != 0) {
            return absl::UnimplementedError(
                absl::StrFormat("%s: a compressed NRO (system 23.0.0+), which this loader does not decode yet", name));
        }
        module.bss = U32(d, 0x38);
        std::memcpy(module.build_id.data(), d.data() + 0x40, 0x20);
        std::uint64_t at = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            Stored& s = stored[i];
            s.file_offset = s.memory_offset = U32(d, 0x20 + 8 * i);
            s.size = s.stored_size = U32(d, 0x24 + 8 * i);
            if (s.size % kPage != 0 || s.memory_offset != at) {
                return invalid(
                    absl::StrFormat("%s: %s segment %s..%s is not whole pages right after the one before it (at %s)",
                                    name, kNames[i], Hex(s.memory_offset), Hex(s.memory_offset + s.size), Hex(at)));
            }
            at += s.size;
        }
        if (at != size) {
            return invalid(
                absl::StrFormat("%s: the segments end at %s, not at the module's size %s", name, Hex(at), Hex(size)));
        }
        if (size > d.size()) {
            return invalid(absl::StrFormat("%s: the module is %s bytes, the file %s: the file is cut short or damaged",
                                           name, Hex(size), Hex(d.size())));
        }
    } else {
        return invalid(absl::StrFormat("%s: not an NSO or NRO", name));
    }
    const std::uint64_t end = stored[2].memory_offset + stored[2].size;
    if (end > d.size() * kLz4MaxRatio) {
        return invalid(absl::StrFormat(
            "%s: the segments span %s bytes, more than a file of %s bytes can fill (a gap of zeros between them)", name,
            Hex(end), Hex(d.size())));
    }
    module.image.assign(end, 0);
    for (std::size_t i = 0; i < 3; ++i) {
        const Stored& s = stored[i];
        const std::string what = absl::StrFormat("%s: %s segment", name, kNames[i]);
        const std::span<const std::uint8_t> raw = d.subspan(s.file_offset, s.stored_size);
        const std::span<std::uint8_t> out(module.image.data() + s.memory_offset, s.size);
        if ((flags >> i & 1) != 0) {
            if ((flags & kNsoZstd) != 0) {
                return absl::UnimplementedError(absl::StrFormat(
                    "%s: a ZBIC (zstd) segment (system 22.0.0+), which this loader does not decode yet", what));
            }
            const std::ptrdiff_t got = internal::Lz4DecompressBlock(raw, out);
            if (got < 0) return absl::DataLossError(absl::StrFormat("%s does not decompress", what));
            if (static_cast<std::uint64_t>(got) != s.size) {
                return absl::DataLossError(
                    absl::StrFormat("%s decompresses to %s bytes, not the %s of the header: the file is damaged", what,
                                    Hex(static_cast<std::uint64_t>(got)), Hex(s.size)));
            }
        } else if (s.size != 0) {
            std::memcpy(out.data(), raw.data(), s.size);
        }
        if ((flags >> (3 + i) & 1) != 0) {
            const fs::Sha256Hash hash = fs::Sha256(out);
            if (std::memcmp(hash.data(), d.data() + 0xa0 + 0x20 * i, hash.size()) != 0) {
                return absl::DataLossError(
                    absl::StrFormat("%s does not match its SHA-256 in the header: the file is damaged", what));
            }
        }
        module.segments[i] = {s.memory_offset, s.size};
    }
    return module;
}

absl::StatusOr<Module> ReadModule(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return absl::NotFoundError(absl::StrFormat("%s: %s", path, std::strerror(errno)));
    const std::streamoff size = in.tellg();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (size != 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {  // NOLINT: bytes as chars
        return absl::DataLossError(absl::StrFormat("%s: cannot read the file", path));
    }
    return LoadModule(bytes, path);
}

}  // namespace nxmac::rewriter
