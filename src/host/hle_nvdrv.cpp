// nvdrv (the NVIDIA driver service), as nxnative's dm/host/hle_nvdrv.c: the host takes over NVN entirely and has no
// use for it; only the SDK movie library's hardware decoder (subsdk0's NVOMX -> NvRmChannelOpen) opens /dev/nvhost-*
// directly. The original NvOsDrvOpen first connects to the nvdrv service; the host has no service process, and the SDK
// aborts on the spot (Engage aborted when Unity created a decoder at startup). Takeover: opening a device always fails
// (fd = -1), and ioctls, mappings and event queries all return an error, so the decoder takes its own failure path.
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hle.h"
#include "log.h"

namespace nxmac::host {
namespace {

constexpr int kNvErr = 3;  // NvError: non-zero is a failure (3: not initialized)
std::atomic<bool> g_open_reported{false};

NXHOST_HLE_FORCE("NvOsDrvOpen", "nvdrv open device: the host has no nvdrv, returns −1", int, h_nvos_Open,
                 const char* path) {
    if (!g_open_reported.exchange(true)) {
        Log("[RED] nvdrv: cannot open %s (the host has no nvdrv; only hardware movie decoding uses it)",
            path != nullptr ? path : "(null)");
    }
    return -1;
}
NXHOST_HLE_FORCE("NvOsDrvClose", "nvdrv close device; nothing to do", void, h_nvos_Close, int /*fd*/) {}
NXHOST_HLE_FORCE("NvOsDrvIoctl", "nvdrv ioctl: returns an error", int, h_nvos_Ioctl, int /*fd*/, std::uint32_t /*req*/,
                 void* /*arg*/, std::size_t /*n*/) {
    return kNvErr;
}
NXHOST_HLE_FORCE("NvOsDrvIoctl2", "nvdrv ioctl2: returns an error", int, h_nvos_Ioctl2, int /*fd*/,
                 std::uint32_t /*req*/, void* /*arg*/, std::size_t /*n*/) {
    return kNvErr;
}
NXHOST_HLE_FORCE("NvOsDrvIoctl3", "nvdrv ioctl3: returns an error", int, h_nvos_Ioctl3, int /*fd*/,
                 std::uint32_t /*req*/, void* /*arg*/, std::size_t /*n*/) {
    return kNvErr;
}
NXHOST_HLE_FORCE("NvOsDrvMapSharedMem", "nvdrv shared memory map: returns an error", int, h_nvos_MapShared,
                 int /*fd*/) {
    return kNvErr;
}
NXHOST_HLE_FORCE("_Z17NvOsDrvQueryEventijPN2nn2os15SystemEventTypeE", "nvdrv query event: returns an error", int,
                 h_nvos_QueryEvent, int /*fd*/, std::uint32_t /*id*/, void* /*ev*/) {
    return kNvErr;
}

}  // namespace
}  // namespace nxmac::host
