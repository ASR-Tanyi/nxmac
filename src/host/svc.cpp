#include "svc.h"

#include <array>
#include <atomic>
#include <cstdlib>

#include "game_profile.h"
#include "log.h"
#include "modules.h"
#include "nxmac/horizon/entropy.h"

namespace nxmac::host {
namespace {

// Horizon's SVCs an application can make, by number
constexpr std::array<std::string_view, 0x56> kNames = [] {
    std::array<std::string_view, 0x56> n{};
    n[0x01] = "SetHeapSize";
    n[0x02] = "SetMemoryPermission";
    n[0x03] = "SetMemoryAttribute";
    n[0x04] = "MapMemory";
    n[0x05] = "UnmapMemory";
    n[0x06] = "QueryMemory";
    n[0x07] = "ExitProcess";
    n[0x08] = "CreateThread";
    n[0x09] = "StartThread";
    n[0x0A] = "ExitThread";
    n[0x0B] = "SleepThread";
    n[0x0C] = "GetThreadPriority";
    n[0x0D] = "SetThreadPriority";
    n[0x0E] = "GetThreadCoreMask";
    n[0x0F] = "SetThreadCoreMask";
    n[0x10] = "GetCurrentProcessorNumber";
    n[0x11] = "SignalEvent";
    n[0x12] = "ClearEvent";
    n[0x13] = "MapSharedMemory";
    n[0x14] = "UnmapSharedMemory";
    n[0x15] = "CreateTransferMemory";
    n[0x16] = "CloseHandle";
    n[0x17] = "ResetSignal";
    n[0x18] = "WaitSynchronization";
    n[0x19] = "CancelSynchronization";
    n[0x1A] = "ArbitrateLock";
    n[0x1B] = "ArbitrateUnlock";
    n[0x1C] = "WaitProcessWideKeyAtomic";
    n[0x1D] = "SignalProcessWideKey";
    n[0x1E] = "GetSystemTick";
    n[0x1F] = "ConnectToNamedPort";
    n[0x20] = "SendSyncRequestLight";
    n[0x21] = "SendSyncRequest";
    n[0x22] = "SendSyncRequestWithUserBuffer";
    n[0x23] = "SendAsyncRequestWithUserBuffer";
    n[0x24] = "GetProcessId";
    n[0x25] = "GetThreadId";
    n[0x26] = "Break";
    n[0x27] = "OutputDebugString";
    n[0x28] = "ReturnFromException";
    n[0x29] = "GetInfo";
    n[0x2A] = "FlushEntireDataCache";
    n[0x2B] = "FlushDataCache";
    n[0x2C] = "MapPhysicalMemory";
    n[0x2D] = "UnmapPhysicalMemory";
    n[0x2F] = "GetLastThreadInfo";
    n[0x30] = "GetResourceLimitLimitValue";
    n[0x31] = "GetResourceLimitCurrentValue";
    n[0x32] = "SetThreadActivity";
    n[0x33] = "GetThreadContext3";
    n[0x34] = "WaitForAddress";
    n[0x35] = "SignalToAddress";
    n[0x36] = "SynchronizePreemptionState";
    n[0x37] = "GetResourceLimitPeakValue";
    n[0x40] = "CreateSession";
    n[0x41] = "AcceptSession";
    n[0x43] = "ReplyAndReceive";
    n[0x45] = "CreateEvent";
    n[0x4B] = "CreateCodeMemory";
    n[0x4C] = "ControlCodeMemory";
    n[0x50] = "CreateSharedMemory";
    n[0x51] = "MapTransferMemory";
    n[0x52] = "UnmapTransferMemory";
    return n;
}();

// GetInfo's types (hle_svc.c I_*: Atmosphère kern_svc_info.cpp's)
enum InfoType : std::uint64_t {
    kCoreMask = 0,
    kPriorityMask = 1,
    kAliasAddr = 2,
    kAliasSize = 3,
    kHeapAddr = 4,
    kHeapSize = 5,
    kTotalMem = 6,
    kUsedMem = 7,
    kDebuggerAttached = 8,
    kResourceLimit = 9,
    kIdleTick = 10,
    kRandomEntropy = 11,
    kAslrAddr = 12,
    kAslrSize = 13,
    kStackAddr = 14,
    kStackSize = 15,
    kSysResTotal = 16,
    kSysResUsed = 17,
    kProgramId = 18,
    kInitProcIdRange = 19,
    kUserExcAddr = 20,
    kTotalNonSys = 21,
    kUsedNonSys = 22,
    kIsApplication = 23,
    kFreeThreadCount = 24,
    kThreadTickCount = 25,
    kIsSvcPermitted = 26,
    kIoRegionHint = 27,
    kAliasExtraSize = 28,
    kRemoteAddr = 29,
    kRemoteSize = 30,
    kRemoteUsage0 = 31,
    kRemoteUsage1 = 32,
    kPageSize = 33,
};

constexpr std::array<std::string_view, 34> kInfoNames = {"CoreMask",
                                                         "PriorityMask",
                                                         "AliasRegionAddress",
                                                         "AliasRegionSize",
                                                         "HeapRegionAddress",
                                                         "HeapRegionSize",
                                                         "TotalMemorySize",
                                                         "UsedMemorySize",
                                                         "DebuggerAttached",
                                                         "ResourceLimit",
                                                         "IdleTickCount",
                                                         "RandomEntropy",
                                                         "AslrRegionAddress",
                                                         "AslrRegionSize",
                                                         "StackRegionAddress",
                                                         "StackRegionSize",
                                                         "SystemResourceSizeTotal",
                                                         "SystemResourceSizeUsed",
                                                         "ProgramId",
                                                         "InitialProcessIdRange",
                                                         "UserExceptionContextAddress",
                                                         "TotalNonSystemMemorySize",
                                                         "UsedNonSystemMemorySize",
                                                         "IsApplication",
                                                         "FreeThreadCount",
                                                         "ThreadTickCount",
                                                         "IsSvcPermitted",
                                                         "IoRegionHint",
                                                         "AliasRegionExtraSize",
                                                         "RemoteRegionAddress",
                                                         "RemoteRegionSize",
                                                         "RemoteUsage0",
                                                         "RemoteUsage1",
                                                         "PageSize"};

std::string_view InfoName(std::uint64_t id) { return id < kInfoNames.size() ? kInfoNames[id] : ""; }

// The guest's entropy (hle_os.c dm_entropy64): a fixed stream with DM_DETCLOCK, the system's otherwise
horizon::Entropy& GuestEntropy() {
    static horizon::Entropy e([] {
        const char* const d = std::getenv("DM_DETCLOCK");
        return horizon::Entropy::Options{.deterministic = d != nullptr && std::strtoull(d, nullptr, 0) > 0,
                                         .log = [](std::string_view line) { Log("%s", line); }};
    }());
    return e;
}

// The program ID GetInfo answers: the profile's program_id, or DM_PROGRAM_ID (hex)
std::uint64_t ProgramId() {
    static const std::uint64_t id = [] {
        const char* const e = std::getenv("DM_PROGRAM_ID");
        return e != nullptr ? std::strtoull(e, nullptr, 16) : std::uint64_t{DM_GAME_PROGRAM_ID};
    }();
    return id;
}

// svc 0x29 GetInfo(out, w1 type, w2 handle, x3 subtype): x1 = the value (hle_svc.c case 0x29). The memory layout's
// types wait for the guest memory layout.
std::uint64_t GetInfo(std::uint64_t* regs) {
    const std::uint64_t id = regs[1];
    std::uint64_t v = 0;
    switch (id) {
        case kCoreMask:
            v = 7;
            break;
        case kTotalMem:
        case kTotalNonSys:
            v = DM_GAME_APP_POOL;  // the profile's app_pool
            break;
        // The system resource the NPDM declares (profile sys_resource). The SDK's IsVirtualAddressMemoryEnabled is
        // "total != 0": Big Brain Academy asks StandardAllocator for 60 GB of virtual address memory when it is
        case kSysResTotal:
            v = DM_GAME_SYS_RESOURCE;
            break;
        case kSysResUsed:
            v = DM_GAME_SYS_RESOURCE != 0 ? 0x400000 : 0;
            break;
        case kRandomEntropy:
            v = GuestEntropy().Next();
            break;
        case kProgramId:
            v = ProgramId();
            break;
        case kIsApplication:
            v = 1;
            break;
        // nn::os::GetFreeThreadCount (SDK 11+): an application's limit of 96 threads (pm) less the threads alive. Only
        // the main thread is, until the thread table comes over (Monster Train 2's Unity player creates threads only
        // while this is at least 1)
        case kFreeThreadCount:
            v = 96 - 1;
            break;
        // The user exception context area: newer SDKs' SetDyingMessageRegion writes to +0x1c0
        case kUserExcAddr: {
            alignas(0x1000) static std::uint8_t ctx[0x1000];
            v = reinterpret_cast<std::uint64_t>(ctx);
            break;
        }
        // The kernel's page size (Switch 2 SDKs): nn::ro::ProtectRelro aligns the RELRO bounds to it
        case kPageSize:
            v = 0x1000;
            break;
        // Answered 0, as Horizon would here: no priority mask, debugger, thread tick count, SVC permission bits, I/O
        // region or extra alias region; no remote region (the Switch 2's GPU boot, which the host replaces)
        case kPriorityMask:
        case kDebuggerAttached:
        case kThreadTickCount:
        case kIsSvcPermitted:
        case kIoRegionHint:
        case kAliasExtraSize:
        case kRemoteAddr:
        case kRemoteSize:
        case kRemoteUsage0:
        case kRemoteUsage1:
            v = 0;
            break;
        case kAliasAddr:
        case kAliasSize:
        case kHeapAddr:
        case kHeapSize:
        case kAslrAddr:
        case kAslrSize:
        case kStackAddr:
        case kStackSize:
        case kUsedMem:
        case kUsedNonSys:
            Die("[RED] svc GetInfo %d %s: needs the guest memory layout, not ported yet", id, InfoName(id));
        default: {
            static std::array<std::atomic<bool>, 64> seen{};
            if (id >= seen.size() || !seen[id].exchange(true)) {
                Log("[RED] svc GetInfo: unknown info type %d (answered 0)", id);
            }
            v = 0;
            break;
        }
    }
    regs[0] = 0;
    regs[1] = v;
    return 0;
}

}  // namespace

std::string_view SvcName(int num) {
    return num >= 0 && static_cast<std::size_t>(num) < kNames.size() ? kNames[static_cast<std::size_t>(num)] : "";
}

std::uint64_t SvcDispatch(int num, std::uint64_t* regs, std::uint64_t orig) {
    switch (num) {
        case 0x29:
            return GetInfo(regs);
        default:
            break;
    }
    // Not ported yet. regs[30] is the gate block's return point, in the module of the svc
    const Module* const m = ModuleOf(regs[30]);
    Die("[RED] svc %#x %s at %s: not ported yet (x0 %#x x1 %#x x2 %#x x3 %#x)", num, SvcName(num),
        m != nullptr ? Where(*m, orig) : Where(regs[30]), regs[0], regs[1], regs[2], regs[3]);
}

}  // namespace nxmac::host
