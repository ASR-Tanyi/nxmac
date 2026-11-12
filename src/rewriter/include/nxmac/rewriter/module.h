// A Switch code module: an NSO (a game's ExeFS: main, sdk, subsdkN) or an NRO (a module a game loads at run time with
// nn::ro), read into its memory image of text + ro + data. As nxnative's dm/tools/nso.py, it accepts what Horizon
// accepts and checks it the same way before anything the header claims is allocated:
//   NSO (Atmosphère's ldr_process_creation.cpp): memory offsets on pages, text at 0, then ro, then data, none
//     overlapping; sizes below 2 GiB; a compressed segment is no larger stored than loaded and decompresses to exactly
//     its size; a segment whose check-hash flag is set matches its SHA-256 in the header.
//   NRO (Atmosphère's ro_service_impl.cpp): sizes in whole pages, text at 0, ro and data right after, data ending at
//     the module's size.
//   Beyond Horizon, so that a small file cannot claim gigabytes: a compressed segment is no more than LZ4 can produce
//     from its stored bytes (255 bytes per byte), and the image no more than that of the whole file.
// LZ4 segments are read; ZBIC ones (Nintendo's zstd variant, system 22.0.0 on) and compressed NROs (23.0.0 on) are
// Unimplemented errors that name them.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace nxmac::rewriter {

struct Segment {
    std::uint64_t offset = 0;  // in memory, from the module's base
    std::uint64_t size = 0;
};

struct Module {
    std::vector<std::uint8_t> image;    // text + ro + data at their memory offsets; bss is only a size
    std::array<Segment, 3> segments{};  // text, ro, data
    std::uint64_t bss = 0;
    std::array<std::uint8_t, 0x20> build_id{};

    std::uint64_t text_end() const { return segments[0].size; }
    // The build ID the patch tables and the host key on: the first 0x10 bytes, 32 lower-case hex digits
    std::string BuildId() const;
};

// name goes into the error messages. InvalidArgument for a file Horizon would not load, DataLoss for one that is
// damaged (a segment that does not decompress or does not match its hash), Unimplemented for ZBIC and compressed NROs.
absl::StatusOr<Module> LoadModule(std::span<const std::uint8_t> file, std::string_view name = "module");
absl::StatusOr<Module> ReadModule(const std::string& path);

}  // namespace nxmac::rewriter
