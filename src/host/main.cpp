// nxhost's entry point, as nxnative's dm/host/main.c: nxhost <module dir> [module...] [-- game arguments...]. So far it
// reads the modules' layout tables, maps their images and stops there: relocating them is the loader's next piece.
#include <pthread.h>

#include <array>
#include <cstdint>
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
#include "nxmac/loader/image.h"
#include "nxmac/loader/layout.h"
#include "nxmac/loader/modules.h"

namespace nxmac::host {
namespace {

constexpr std::size_t kMaxGameArgs = 63;  // as main.c's g_argv

// The four thread slots (dm.h), in key_idx's order: K18 = the Switch's x18, KTLS = tpidr_el0, KTLR = tpidrro_el0 (the
// ThreadLocalRegion), KGATE = the host gate's entry. Every module's key page holds their slot offsets.
std::array<pthread_key_t, loader::kKeyCount> g_keys{};

// loader.c create_keys
loader::HostKeys CreateKeys() {
    for (pthread_key_t& k : g_keys) {
        if (pthread_key_create(&k, nullptr) != 0) Die("cannot create the host key");
    }
    Log("key numbers K18=%lu KTLS=%lu KTLR=%lu KGATE=%lu", static_cast<unsigned long>(g_keys[loader::kKeyK18]),
        static_cast<unsigned long>(g_keys[loader::kKeyTls]), static_cast<unsigned long>(g_keys[loader::kKeyTlr]),
        static_cast<unsigned long>(g_keys[loader::kKeyGate]));
    loader::HostKeys keys{};
    for (int k = 0; k < loader::kKeyCount; ++k) keys[k] = static_cast<std::uint64_t>(g_keys[k]);
    return keys;
}

// The K slots' check on the first module (loader.c tsd_check): pthread's TSD layout is fixed, a key's slot at
// tpidrro_el0
// + 8 * key, where the rewritten code reads it through the key page
void TsdCheck(const loader::ModuleImage& m, const loader::ModuleLayout& l) {
#if defined(__APPLE__) && defined(__aarch64__)
    pthread_setspecific(g_keys[loader::kKeyK18], reinterpret_cast<void*>(0x1234));
    std::uint64_t base = 0;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    const std::uint64_t off =
        *reinterpret_cast<const std::uint64_t*>(m.base() + l.key_page_off + l.key_idx[loader::kKeyK18] * 8);
    if (reinterpret_cast<const std::uint64_t*>(base & ~std::uint64_t{7})[off / 8] != 0x1234) Die("TSD layout mismatch");
    pthread_setspecific(g_keys[loader::kKeyK18], nullptr);
#else
    (void)m;  // nxhost runs on Apple's arm64 alone
    (void)l;
#endif
}

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
    // load_modules: every module's layout first, then the host keys, then each image mapped (its code sealed on return)
    std::vector<loader::ModuleLayout> layouts;
    for (const std::string& name : names) {
        absl::StatusOr<loader::ModuleLayout> m = loader::ReadLayout(dir, name);
        if (!m.ok()) Die("%s", m.status().message());
        Log("%s: patch area %s, key page %#x, text end %#x, ro %#x, data %#x, build %s", name, SignedHex(m->patch_off),
            m->key_page_off, m->text_end, m->ro_off, m->data_off, m->build_id.empty() ? "-" : m->build_id);
        layouts.push_back(*std::move(m));
    }
    const loader::HostKeys keys = CreateKeys();
    std::vector<loader::ModuleImage> images;
    for (std::size_t k = 0; k < names.size(); ++k) {
        absl::StatusOr<loader::ModuleImage> m = loader::MapModule(dir, names[k], layouts[k], keys);
        if (!m.ok()) Die("%s", m.status().message());
        if (k == 0) TsdCheck(*m, layouts[k]);
        Log("%s: base %p size %#x", names[k], static_cast<const void*>(m->base()), m->size());
        images.push_back(*std::move(m));
    }
    Log("[RED] nxhost stops here: relocating the modules is not ported yet");
    return 1;
}

}  // namespace
}  // namespace nxmac::host

int main(int argc, char** argv) { return nxmac::host::Main(argc, argv); }
