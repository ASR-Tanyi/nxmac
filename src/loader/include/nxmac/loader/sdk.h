// Which SDK the game's sdk module was built from, read from its read-only data, data and bss (from RO_OFF up to the key
// page): its version string ("NintendoSDK_nnSdk-20.5.22-Release" or with a variant, "...nnSdk-Generic_16.2.4...") and
// whether it was built for the Switch 2 (its file system access log line says "spec: Ounce"; Switch 1 SDKs say "spec:
// NX"). SDK-dependent behaviour follows them: the system counter's rate, among others. A port of nxnative's
// dm/host/loader.c dm_sdk_major and dm_sdk_ounce.
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace nxmac::loader {

struct SdkVersion {
    int major = 0;     // 0: no version string (SDK-dependent behaviour then takes the newer SDKs' side)
    std::string text;  // what follows "nnSdk-", at most 20 characters (nxnative's log line shows that much)
};

// The first "NintendoSDK" (any case) "_nnSdk-" [variant "_"] <digits> in `data`
SdkVersion FindSdkVersion(std::span<const std::uint8_t> data);

// Whether `data` holds "spec: Ounce"
bool IsOunce(std::span<const std::uint8_t> data);

}  // namespace nxmac::loader
