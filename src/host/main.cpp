// nxhost's entry point, as nxnative's dm/host/main.c: nxhost <module dir> [module...] [-- game arguments...]. So far it
// reads the modules' layout tables and stops there: mapping them is the loader's next piece.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_split.h"
#include "game_profile.h"
#include "log.h"
#include "nxmac/core/version.h"
#include "nxmac/gfx/metal_device.h"
#include "nxmac/loader/layout.h"
#include "nxmac/loader/modules.h"

namespace nxmac::host {
namespace {

constexpr std::size_t kMaxGameArgs = 63;  // as main.c's g_argv

// A signed offset in hex: -0x2000000 for a patch area before the base
std::string SignedHex(std::int64_t v) {
    return absl::StrFormat("%s%#x", v < 0 ? "-" : "",
                           v < 0 ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v));
}

int Main(int argc, char** argv) {
    if (argc < 2) Die("usage: nxhost <dir> [mod...] [-- game arguments...]");
    const std::string gpu = gfx::DefaultGpuArchitecture();
    Log("build: nxhost %s, profile %s (%s), GPU %s", kVersion, DM_GAME_ID, DM_GAME_NAME, gpu.empty() ? "none" : gpu);

    // The game's arguments: "game", then what follows "--" on the command line, else the profile's game_args (on
    // Horizon nn::init takes them from the process's argument area and hands them to the SDK, which the boot will do)
    std::vector<std::string> game_args = {"game"};
    std::vector<std::string> names;
    int i = 2;
    for (; i < argc && std::string_view(argv[i]) != "--"; ++i) names.emplace_back(argv[i]);
    if (i < argc) {
        for (++i; i < argc && game_args.size() < kMaxGameArgs; ++i) game_args.emplace_back(argv[i]);
    } else if (*DM_GAME_ARGS) {  // no "--": the profile's game_args, split at spaces
        for (const std::string_view arg : absl::StrSplit(DM_GAME_ARGS, ' ', absl::SkipEmpty())) {
            if (game_args.size() == kMaxGameArgs) break;
            game_args.emplace_back(arg);
        }
        Log("game arguments from the game profile: %s", DM_GAME_ARGS);
    }

    const std::filesystem::path dir = argv[1];
    if (names.empty()) {
        static constexpr const char* kModules[] = DM_GAME_MODULES;
        static constexpr loader::BuildModules kRows[] = {DM_GAME_BUILD_MODULES};
        loader::ModuleChoice choice = loader::ProfileModules(dir, kModules, kRows);
        if (!choice.note.empty()) Log("%s", choice.note);
        names = std::move(choice.names);
    }
    // load_modules' first pass: every module's layout, before anything is mapped
    for (const std::string& name : names) {
        const absl::StatusOr<loader::ModuleLayout> m = loader::ReadLayout(dir, name);
        if (!m.ok()) Die("%s", m.status().message());
        Log("%s: patch area %s, key page %#x, text end %#x, ro %#x, data %#x, build %s", name, SignedHex(m->patch_off),
            m->key_page_off, m->text_end, m->ro_off, m->data_off, m->build_id.empty() ? "-" : m->build_id);
    }
    Log("[RED] nxhost stops here: mapping the modules is not ported yet");
    return 1;
}

}  // namespace
}  // namespace nxmac::host

int main(int argc, char** argv) { return nxmac::host::Main(argc, argv); }
