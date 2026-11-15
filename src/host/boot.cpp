#include "boot.h"

#include <pthread.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "gate.h"
#include "log.h"
#include "modules.h"
#include "thread.h"

namespace nxmac::host {
namespace {

using Fn0 = void (*)();

// main.c req: a symbol the boot calls must be there
template <typename F>
F Req(std::string_view name) {
    std::uint8_t* const p = GuestSym(name);
    if (p == nullptr) Die("missing symbol %s", name);
    return reinterpret_cast<F>(p);
}

// The game's arguments as the SDK keeps them: argv's pointers and strings live as long as the process
std::vector<std::string> g_arg_text;
std::array<char*, 64> g_argv{};

// rtld's callback B (loader.c run_module_inits): each module's DT_INIT in load order, between nninitStartup and nnMain
void RunModuleInits() {
    for (const Module& m : Modules()) {
        if (m.symbols.init() == 0) continue;
        Log("%s: DT_INIT +%#x (init_array %u entries)", m.name, m.symbols.init(), m.symbols.init_arraysz() / 8);
        reinterpret_cast<Fn0>(m.image.base() + m.symbols.init())();
    }
}

// svc_thread_main_init, until the thread table comes over: the handle and ID nxnative's gives the main thread (the
// first handle, the first ID)
constexpr std::uint32_t kMainThreadHandle = 0x1001;
constexpr std::uint64_t kMainThreadId = 0x51;

std::uint32_t MainThreadInit() {
    GuestThreadSetId(kMainThreadId);
#if defined(__APPLE__)
    pthread_setname_np("0x1001 main thread");
#endif
    Log("[RED] main guest thread: handle %#x, ID %#x, without the thread table yet (hle_thread.c)", kMainThreadHandle,
        kMainThreadId);
    return kMainThreadHandle;
}

}  // namespace

void GuestMain(const std::vector<std::string>& args) {
    GuestThreadInit();
    GuestGateStack(
        /*own_stack=*/false);  // the guest code runs on this thread's own stack: its SVCs get one of their own
    const std::uint32_t hmain = MainThreadInit();
    // nn::init::Start's steps: _init_libc1 initializes the C library's locks as recursive ones (without it the first
    // nested lock in putc deadlocks); rtld's callback A only sets a flag of its own (skipped)
    Req<Fn0>("_init_libc0")();
    Log(">>> nnosInitialize");
    Req<void (*)(std::uint64_t, std::uint64_t)>("nnosInitialize")(hmain, 0);
    Req<Fn0>("_init_libc1")();
    Log(">>> nninitInitializeSdkModule");
    Req<Fn0>("nninitInitializeSdkModule")();
    // The game's arguments before nninitStartup, as on Horizon, where nn::os takes them from the process's argument
    // area in nnosInitialize (Unreal Engine's nninitStartup reads its memory options there)
    g_arg_text = args;
    std::size_t argc = 0;
    for (std::string& a : g_arg_text) {
        if (argc + 1 == g_argv.size()) break;
        g_argv[argc++] = a.data();
    }
    Req<void (*)(std::uint64_t)>("_ZN2nn2os11SetHostArgcEi")(argc);
    Req<void (*)(char**)>("_ZN2nn2os11SetHostArgvEPPc")(g_argv.data());
    Log(">>> nninitStartup");
    Req<Fn0>("nninitStartup")();
    Req<Fn0>("_init_libc2")();
    RunModuleInits();
    Log(">>> nnMain");
    Req<Fn0>("nnMain")();
    Log("nnMain returned");
}

}  // namespace nxmac::host
