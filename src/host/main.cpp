// nxhost's entry point, as nxnative's dm/host/main.c: nxhost <module dir> [module...] [-- game arguments...]. So far it
// reads the modules' layout tables, maps their images, reads their symbols, registers their host gates, relocates
// them and finds the boot's symbols, then stops there: the gate's dispatch and the host's implementations come next.
#include <dlfcn.h>
#include <pthread.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "game_profile.h"
#include "gate.h"
#include "hle.h"
#include "keys.h"
#include "log.h"
#include "nxmac/core/version.h"
#include "nxmac/gfx/metal_device.h"
#include "nxmac/loader/gates.h"
#include "nxmac/loader/hle.h"
#include "nxmac/loader/image.h"
#include "nxmac/loader/layout.h"
#include "nxmac/loader/modules.h"
#include "nxmac/loader/relocate.h"
#include "nxmac/loader/symbols.h"

namespace nxmac::host {
namespace {

constexpr std::size_t kMaxGameArgs = 63;  // as main.c's g_argv

// A signed offset in hex: -0x2000000 for a patch area before the base
std::string SignedHex(std::int64_t v) {
    return absl::StrFormat("%s%#x", v < 0 ? "-" : "",
                           v < 0 ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v));
}

// One loaded module (dm.h module_t): its table, its image, its symbols
struct Module {
    std::string name;
    loader::ModuleLayout layout;
    loader::ModuleImage image;
    loader::ModuleSymbols symbols;
};
std::vector<Module> g_modules;               // in load order; filled once, before any guest code runs
std::unique_ptr<loader::GateTable> g_gates;  // every module's host gates, by return point (hle.c g_gate)

// The first module, in load order, that defines `name` (loader.c guest_sym / loaded_sym); null when none does
std::uint8_t* GuestSym(std::string_view name) {
    for (const Module& m : g_modules) {
        if (std::uint8_t* const p = m.symbols.Find(name)) return p;
    }
    return nullptr;
}

// The module `p` is in, a far module's front patch area included (loader.c module_of); null when it is in none
const Module* ModuleOf(const void* p) {
    const auto* const a = static_cast<const std::uint8_t*>(p);
    for (const Module& m : g_modules) {
        const std::uint8_t* const b = m.image.base();
        if (a >= b && a < b + m.image.size()) return &m;
        if (m.layout.patch_off < 0 && a >= b + m.layout.patch_off && a < b + m.layout.patch_off + m.image.patch_len()) {
            return &m;
        }
    }
    return nullptr;
}

// "main+0x12afc" for an address in a module
std::string Where(const void* p) {
    const Module* const m = ModuleOf(p);
    if (m == nullptr) return absl::StrFormat("%p", p);
    const std::int64_t off = static_cast<const std::uint8_t*>(p) - m->image.base();
    return absl::StrCat(m->name, off < 0 ? "-" : "+", SignedHex(off < 0 ? -off : off));
}

// What the boot calls by name, in its order (main.c guest_main: the steps of nn::init::Start, then nnMain)
constexpr std::string_view kBootSymbols[] = {"_init_libc0",
                                             "nnosInitialize",
                                             "_init_libc1",
                                             "nninitInitializeSdkModule",
                                             "_ZN2nn2os11SetHostArgcEi",
                                             "_ZN2nn2os11SetHostArgvEPPc",
                                             "nninitStartup",
                                             "_init_libc2",
                                             "nnMain"};

// The K slots' check on the first module (loader.c tsd_check): pthread's TSD layout is fixed, a key's slot at
// tpidrro_el0
// + 8 * key, where the rewritten code reads it through the key page
void TsdCheck(const loader::ModuleImage& m, const loader::ModuleLayout& l) {
#if defined(__APPLE__) && defined(__aarch64__)
    pthread_setspecific(Keys()[loader::kKeyK18], reinterpret_cast<void*>(0x1234));
    std::uint64_t base = 0;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    const std::uint64_t off =
        *reinterpret_cast<const std::uint64_t*>(m.base() + l.key_page_off + l.key_idx[loader::kKeyK18] * 8);
    if (reinterpret_cast<const std::uint64_t*>(base & ~std::uint64_t{7})[off / 8] != 0x1234) Die("TSD layout mismatch");
    pthread_setspecific(Keys()[loader::kKeyK18], nullptr);
#else
    (void)m;  // nxhost runs on Apple's arm64 alone
    (void)l;
#endif
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
    // and its dynamic section read
    g_modules.reserve(names.size());
    for (const std::string& name : names) {
        absl::StatusOr<loader::ModuleLayout> m = loader::ReadLayout(dir, name);
        if (!m.ok()) Die("%s", m.status().message());
        Log("%s: patch area %s, key page %#x, text end %#x, ro %#x, data %#x, build %s", name, SignedHex(m->patch_off),
            m->key_page_off, m->text_end, m->ro_off, m->data_off, m->build_id.empty() ? "-" : m->build_id);
        g_modules.push_back({name, *std::move(m), {}, {}});
    }
    const loader::HostKeys keys = CreateKeys();
    g_gates = std::make_unique<loader::GateTable>();
    for (std::size_t k = 0; k < g_modules.size(); ++k) {
        Module& m = g_modules[k];
        absl::StatusOr<loader::ModuleImage> image = loader::MapModule(dir, m.name, m.layout, keys);
        if (!image.ok()) Die("%s", image.status().message());
        m.image = *std::move(image);
        if (k == 0) TsdCheck(m.image, m.layout);
        absl::StatusOr<loader::ModuleSymbols> symbols =
            loader::ModuleSymbols::Parse(m.image.base(), m.image.size(), m.name);
        if (!symbols.ok()) Die("%s", symbols.status().message());
        m.symbols = *std::move(symbols);
        const absl::StatusOr<int> gates =
            loader::RegisterGates(*g_gates, dir, m.name, reinterpret_cast<std::uint64_t>(m.image.base()));
        if (!gates.ok()) Die("%s", gates.status().message());
        Log("%s: gates %d patch_off %#x keys %d/%d/%d/%d", m.name, *gates,
            static_cast<std::uint64_t>(m.layout.patch_off), keys[loader::kKeyK18], keys[loader::kKeyTls],
            keys[loader::kKeyTlr], keys[loader::kKeyGate]);
        Log("%s: base %p size %#x", m.name, static_cast<const void*>(m.image.base()), m.image.size());
        Log("%s: %u symbols (%u hash slots), DT_INIT %#x, init_array %u entries", m.name, m.symbols.nsym(),
            m.symbols.hash_slots(), m.symbols.init(), m.symbols.init_arraysz() / 8);
    }
    // Relocation, once every module is mapped: a symbol can bind to any of them, or to nxhost's takeovers (the
    // hle_*.cpp files). The named stubs' code (entry.S's table) is not there yet: an import nothing defines or
    // implements would stay 0.
    loader::HleOptions options;
    options.log_imports = std::getenv("DM_IMPORTS") != nullptr;
    options.log = [](std::string_view line) { Log("%s", line); };
    options.describe = [](const void* fn) {
        Dl_info info{};
        return dladdr(fn, &info) != 0 && info.dli_sname != nullptr ? std::string(info.dli_sname) : std::string("?");
    };
    absl::StatusOr<loader::HleResolver> hle = loader::HleResolver::Create(HleTable(), ForceTable(), std::move(options));
    if (!hle.ok()) Die("%s", hle.status().message());
    std::vector<loader::LoadedModule> loaded;
    loaded.reserve(g_modules.size());
    for (const Module& m : g_modules) {
        loaded.push_back({m.name, m.image.base(), m.image.size(), m.layout.patch_off, m.image.patch_len(), &m.symbols});
    }
    for (std::size_t k = 0; k < loaded.size(); ++k) {
        const absl::StatusOr<loader::RelocationReport> r = loader::Relocate(loaded, k, *hle);
        if (!r.ok()) Die("%s", r.status().message());
        for (const std::string& line : r->lines) Log("%s", line);
    }
    if (absl::Status s = hle->CheckForced(std::getenv("DM_FORCE_STRICT") != nullptr); !s.ok()) Die("%s", s.message());
    Log("stubs=%u", hle->stubs());  // stubs_report
    // The gates, with nothing behind them yet: a gate the guest reaches stops with a [RED] line for its kind
    SetGates(g_gates.get(), {});

    // main.c req: what the boot calls must be there
    std::string boot;
    for (const std::string_view name : kBootSymbols) {
        const std::uint8_t* const p = GuestSym(name);
        if (p == nullptr) Die("missing symbol %s", name);
        absl::StrAppend(&boot, boot.empty() ? "" : ", ", name, " ", Where(p));
    }
    Log("boot symbols: %s", boot);
    Log("[RED] nxhost stops here: the SVCs, the guest clock, most takeovers and the named stubs' code are not ported "
        "yet "
        "(%d gates, %u stubs)",
        g_gates->count(), hle->stubs());
    return 1;
}

}  // namespace
}  // namespace nxmac::host

int main(int argc, char** argv) { return nxmac::host::Main(argc, argv); }
