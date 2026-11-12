// How the rewriter stops (internal to nxmac::rewriter). nxnative's rewriter is a script whose assertions end it; the
// same checks sit deep inside the code that assembles stubs and reads jump tables, so here they throw RewriteError,
// which Rewrite() catches and returns as a Status. Nothing else in nxmac throws.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace nxmac::rewriter::internal {

class RewriteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A function rule does not match the module (nxnative's nso_match.MatchError): the rule is skipped, not the rewrite
class MatchError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void Fail(const std::string& message);
[[noreturn]] void FailBranch(const char* what, std::int64_t from, std::int64_t to);

}  // namespace nxmac::rewriter::internal
