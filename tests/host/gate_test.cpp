// The host gate end to end, on an Apple arm64 Mac: the rewriter's fixture sys.nso rewritten as nso writes it, mapped
// and its gates registered by nxmac::loader, then its functions called natively. Their svc instructions are gate blocks
// now, which reach a handler here through the thread's KGATE slot, its gate stub and gate.S, and come back.
#include "gate.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <vector>

#include "absl/status/statusor.h"
#include "gtest/gtest.h"
#include "keys.h"
#include "nxmac/loader/gates.h"
#include "nxmac/loader/image.h"
#include "nxmac/loader/layout.h"
#include "nxmac/loader/symbols.h"
#include "nxmac/rewriter/module.h"
#include "nxmac/rewriter/rewrite.h"

namespace nxmac::host {
namespace {

struct Seen {
    int num;
    std::uint64_t x0;
    std::uint64_t x8;
    std::uint64_t orig;
};
std::vector<Seen> g_seen;  // what the handler was called with, in order (one test thread at a time)

std::uint64_t g_ticks = 0;  // the counter reads so far; each one reads the next value
std::uint64_t TestTicks() { return 0x1000 + ++g_ticks; }

std::uint64_t TestSvc(int num, std::uint64_t* regs, std::uint64_t orig) {
    g_seen.push_back({num, regs[0], regs[8], orig});
    if (num == 0x1f) regs[0] += 1;
    if (num == 0x7) regs[0] = 100;
    if (num == 0x7f) regs[0] += 1;
    return 0;
}

// sys.nso rewritten, mapped, its symbols read and its gates registered, once for the suite
struct Module {
    loader::ModuleImage image;
    loader::ModuleSymbols symbols;
    loader::GateTable gates;
};

class GateTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        const loader::HostKeys keys = CreateKeys();
        const absl::StatusOr<rewriter::Module> nso =
            rewriter::ReadModule((std::filesystem::path(NXMAC_REWRITER_FIXTURES) / "sys.nso").string());
        ASSERT_TRUE(nso.ok()) << nso.status();
        rewriter::RewriteOptions options;
        options.name = "sdk";
        const absl::StatusOr<rewriter::RewriteResult> result = rewriter::Rewrite(*nso, options);
        ASSERT_TRUE(result.ok()) << result.status();
        const std::filesystem::path dir = std::filesystem::path(::testing::TempDir()) / "gate_test";
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "sdk.patched.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(result->patched.data()),
                   static_cast<std::streamsize>(result->patched.size()));
        std::ofstream(dir / "sdk.gates.tsv") << result->gates;
        const absl::StatusOr<loader::ModuleLayout> layout = loader::ParseLayout(result->fixups, "sdk.fixups.tsv");
        ASSERT_TRUE(layout.ok()) << layout.status();
        absl::StatusOr<loader::ModuleImage> image = loader::MapModule(dir, "sdk", *layout, keys);
        ASSERT_TRUE(image.ok()) << image.status();
        absl::StatusOr<loader::ModuleSymbols> symbols =
            loader::ModuleSymbols::Parse(image->base(), image->size(), "sdk");
        ASSERT_TRUE(symbols.ok()) << symbols.status();
        module_ = new Module{*std::move(image), *std::move(symbols), {}};
        const absl::StatusOr<int> n =
            loader::RegisterGates(module_->gates, dir, "sdk", reinterpret_cast<std::uint64_t>(module_->image.base()));
        ASSERT_TRUE(n.ok()) << n.status();
        ASSERT_GE(*n, 3);  // svc #0x1f, svc #0x7, svc #0x7f, and the counter reads
        GateHandlers handlers;
        handlers.svc = &TestSvc;
        handlers.ticks = &TestTicks;
        SetGates(&module_->gates, handlers);
    }

    void SetUp() override {
        ASSERT_NE(module_, nullptr);
        g_seen.clear();
        g_ticks = 0;
    }

    template <typename Fn>
    static Fn Function(const char* name) {
        std::uint8_t* const p = module_->symbols.Find(name);
        EXPECT_NE(p, nullptr) << name;
        return reinterpret_cast<Fn>(p);
    }
    static std::uint64_t Offset(const char* name) { return module_->symbols.Find(name) - module_->image.base(); }

    static inline Module* module_ = nullptr;  // never freed: its gates stay registered for the process
};

TEST_F(GateTest, AnSvcGoesThroughTheGateAndBack) {
    GuestGateStack(false);
    const auto sys_svc = Function<std::uint64_t (*)(std::uint64_t)>("sys_svc");  // svc #0x1f; ret
    EXPECT_EQ(sys_svc(41), 42u);
    ASSERT_EQ(g_seen.size(), 1u);
    EXPECT_EQ(g_seen[0].num, 0x1f);
    EXPECT_EQ(g_seen[0].x0, 41u);
    EXPECT_EQ(g_seen[0].orig, Offset("sys_svc"));  // the svc's own offset in the module
}

TEST_F(GateTest, TwoInARowSeeTheRegistersAsTheGuestLeftThem) {
    GuestGateStack(false);
    const auto sys_svc2 = Function<std::uint64_t (*)(std::uint64_t)>("sys_svc2");  // mov x8, x0; svc #7; svc #0x7f; ret
    EXPECT_EQ(sys_svc2(5), 101u);
    ASSERT_EQ(g_seen.size(), 2u);
    EXPECT_EQ(g_seen[0].num, 0x7);
    EXPECT_EQ(g_seen[0].x8, 5u);
    EXPECT_EQ(g_seen[1].num, 0x7f);
    EXPECT_EQ(g_seen[1].x0, 100u);  // what the first gate wrote back
    EXPECT_EQ(g_seen[1].x8, 5u);
    EXPECT_EQ(g_seen[1].orig, g_seen[0].orig + 4);
}

TEST_F(GateTest, EachThreadHasItsOwnGateStack) {
    GuestGateStack(false);
    const auto sys_svc = Function<std::uint64_t (*)(std::uint64_t)>("sys_svc");
    std::optional<std::uint64_t> other;
    std::thread t([&] {
        GuestGateStack(false);
        other = sys_svc(1000);
    });
    t.join();
    EXPECT_EQ(other, 1001u);
    EXPECT_EQ(sys_svc(7), 8u);  // this thread's gate still works after the other's
}

// sys_timers: mrs x9, cntpct; mrs x18, cntpct; mrs x1, cntvct; mrs x2, ctr; mrs x18, ctr; mrs x30, cntpct; ret. Four
// counter reads through the gate; the one into x30 cannot reach the guest and must not take the gate's return with it
TEST_F(GateTest, CounterReadsGoThroughTheGate) {
    GuestGateStack(false);
    const auto sys_timers = Function<void (*)()>("sys_timers");
    sys_timers();
    EXPECT_EQ(g_ticks, 4u);
    EXPECT_TRUE(g_seen.empty());  // no svc among them
}

}  // namespace
}  // namespace nxmac::host
