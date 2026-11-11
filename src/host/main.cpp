// nxhost's entry point. For now it reports what it was built for; the loader, the SVC gate and the services come over
// from nxnative's dm/host one at a time.
#include <string>
#include <string_view>

#include "absl/strings/str_format.h"
#include "game_profile.h"
#include "nxmac/core/version.h"
#include "nxmac/gfx/metal_device.h"

int main() {
    const std::string gpu = nxmac::gfx::DefaultGpuArchitecture();
    absl::PrintF("nxhost %s for %s (%s), GPU %s\n", nxmac::kVersion, DM_GAME_ID, DM_GAME_NAME,
                 gpu.empty() ? std::string_view("none") : std::string_view(gpu));
    return 0;
}
