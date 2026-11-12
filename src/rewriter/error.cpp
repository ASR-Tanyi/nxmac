#include "error.h"

#include "absl/strings/str_format.h"
#include "text.h"

namespace nxmac::rewriter::internal {

void Fail(const std::string& message) { throw RewriteError(message); }

void FailBranch(const char* what, std::int64_t from, std::int64_t to) {
    throw RewriteError(absl::StrFormat("%s at %s does not reach %s", what, Hex(from), Hex(to)));
}

}  // namespace nxmac::rewriter::internal
