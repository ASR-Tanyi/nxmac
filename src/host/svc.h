// The SVCs the guest makes through the gate (nxnative's dm/host/hle_svc.c svc_dispatch, with hle_thread.c svc_thread
// for the threads' and synchronization's). They come over one at a time, in the order the boot meets them: an SVC not
// ported yet stops with a [RED] line that names it, where the guest made it and its first arguments.
#pragma once

#include <cstdint>
#include <string_view>

namespace nxmac::host {

// The gate's svc handler: svc #num with the guest's registers (x0..x7 the arguments; results written back), `orig`
// the svc's offset in its module
std::uint64_t SvcDispatch(int num, std::uint64_t* regs, std::uint64_t orig);

// Horizon's name for an SVC number ("" for one it does not have)
std::string_view SvcName(int num);

}  // namespace nxmac::host
