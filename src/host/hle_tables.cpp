// The takeover tables, read back from nxhost's own image (nxnative's hle.c hle_find and loader.c force_init)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

#include "hle.h"

namespace nxmac::host {
namespace {

template <typename T>
std::span<const T> Section(const char* name) {
    unsigned long size = 0;
    const std::uint8_t* const data = getsectiondata(&_mh_execute_header, "__DATA", name, &size);
    if (data == nullptr) return {};
    return {reinterpret_cast<const T*>(data), size / sizeof(T)};
}

}  // namespace

std::span<const loader::HleEntry> HleTable() { return Section<loader::HleEntry>("swhle"); }
std::span<const loader::HleForceEntry> ForceTable() { return Section<loader::HleForceEntry>("swforce"); }

}  // namespace nxmac::host
