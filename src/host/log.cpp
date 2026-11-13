#include "log.h"

#include <cstdio>
#include <cstdlib>

#include "absl/strings/str_cat.h"

namespace nxmac::host {

void LogLine(std::string_view message) {
    std::FILE* const f = stderr;
    flockfile(f);
    std::fputs("[dm] ", f);
    std::fwrite(message.data(), 1, message.size(), f);
    std::fputc('\n', f);
    funlockfile(f);
}

void DieLine(std::string_view message) {
    LogLine(absl::StrCat("FATAL ", message));
    std::abort();
}

}  // namespace nxmac::host
