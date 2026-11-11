// Little- and big-endian loads and stores of the formats' fields (internal to nxmac::fs). Callers check the buffer's
// size first.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace nxmac::fs::internal {

template <typename T>
T LoadLe(std::span<const std::uint8_t> bytes, std::size_t offset) {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(static_cast<T>(bytes[offset + i]) << (8 * i));
    return value;
}

template <typename T>
void StoreLe(std::span<std::uint8_t> bytes, std::size_t offset, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

inline void StoreBe64(std::uint8_t* out, std::uint64_t value) {
    for (int i = 7; i >= 0; --i, value >>= 8) out[i] = static_cast<std::uint8_t>(value);
}

constexpr std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

}  // namespace nxmac::fs::internal
