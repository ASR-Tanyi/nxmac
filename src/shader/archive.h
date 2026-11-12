// The containers the split opens on the way to shaders (internal to nxmac::shader): Yaz0, SARC, gzip, zlib streams
// (shader_split.py's yaz0, sarc_members, ungzip, gust_gz, frame_size and the zlib calls of its readers), and UTF-8
// as Python decodes it.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "py.h"

namespace nxmac::shader::internal {

inline constexpr std::int64_t kNest = 16;  // archives in archives (scan_blob's NEST)

// Yaz0 decompression; red() naming label when the stream ends early or points before its start
std::string Yaz0(Bytes s, const std::string& label);

// Each member of a SARC, (name, bytes), to f in order, as shader_split.py's generator yields them: a member's range is
// checked when its turn comes. A member without a name is #<index>.
void SarcMembers(Bytes d, const std::string& label, const std::function<void(const std::string&, Bytes)>& f);

// A gzip file's members, decoded, bounded as ungzip bounds them
std::string Ungzip(Bytes d, const std::string& label);

// Gust's chunked zlib, or nullopt when d is not in that form
std::optional<std::string> GustGz(Bytes d);

// repr(bytes), as Python prints it in a message
std::string PyBytesRepr(Bytes b);

// The uncompressed size a zstd or LZ4 frame header records
std::optional<std::uint64_t> FrameSize(Bytes f);

// zlib.decompressobj().decompress(in, max_out) (max_out 0: no limit): what it gives, whether the stream ended and
// what follows it. A stream zlib cannot decode is a zlib.error (PyError).
struct Inflated {
    std::string out;
    bool eof = false;
    std::size_t unused = 0;  // bytes of in after the stream's end
};
Inflated Inflate(Bytes in, std::size_t max_out = 0);
// The same, nullopt for a zlib.error
std::optional<Inflated> TryInflate(Bytes in, std::size_t max_out = 0);

// bytes.decode('utf-8', 'replace') as UTF-8 again; and whether bytes.decode('utf-8') succeeds
std::string DecodeUtf8Replace(Bytes d);
bool ValidUtf8(Bytes d);

}  // namespace nxmac::shader::internal
