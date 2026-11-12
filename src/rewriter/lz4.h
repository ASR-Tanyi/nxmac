// LZ4 block decompression (the format of an NSO's compressed segments), internal to nxmac::rewriter.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace nxmac::rewriter::internal {

// Decompresses one LZ4 block into out and returns the bytes written, which may be fewer than out.size() (a stream cut
// short at a sequence boundary decodes without an error: callers compare the size), or -1 for a block that is not
// LZ4 or that holds more than out.size() bytes. Reads and writes stay inside in and out for any input.
std::ptrdiff_t Lz4DecompressBlock(std::span<const std::uint8_t> in, std::span<std::uint8_t> out);

}  // namespace nxmac::rewriter::internal
