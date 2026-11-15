#include "svc.h"

#include <array>

#include "log.h"
#include "modules.h"

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

}  // namespace

std::string_view SvcName(int num) {
    return num >= 0 && static_cast<std::size_t>(num) < kNames.size() ? kNames[static_cast<std::size_t>(num)] : "";
}

std::uint64_t SvcDispatch(int num, std::uint64_t* regs, std::uint64_t orig) {
    // regs[30] is the gate block's return point, in the module of the svc
    const Module* const m = ModuleOf(regs[30]);
    Die("[RED] svc %#x %s at %s: not ported yet (x0 %#x x1 %#x x2 %#x x3 %#x)", num, SvcName(num),
        m != nullptr ? Where(*m, orig) : Where(regs[30]), regs[0], regs[1], regs[2], regs[3]);
}

}  // namespace nxmac::host
