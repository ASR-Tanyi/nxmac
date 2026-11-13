// nxhost's log, as nxnative's dm/host/dm.h LOG and die: one line on stderr, "[dm] <message>", written under the
// stream's lock so that another thread's line never lands inside it; Die logs "[dm] FATAL <message>" and aborts.
#pragma once

#include <string_view>

#include "absl/strings/str_format.h"

namespace nxmac::host {

void LogLine(std::string_view message);
[[noreturn]] void DieLine(std::string_view message);

template <typename... Args>
void Log(const absl::FormatSpec<Args...>& format, const Args&... args) {
    LogLine(absl::StrFormat(format, args...));
}

template <typename... Args>
[[noreturn]] void Die(const absl::FormatSpec<Args...>& format, const Args&... args) {
    DieLine(absl::StrFormat(format, args...));
}

}  // namespace nxmac::host
