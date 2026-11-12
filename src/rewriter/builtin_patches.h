// The patch tables built into the library: the text of src/rewriter/patches.tsv (internal to nxmac::rewriter; the
// build writes the definition, see CMakeLists.txt).
#pragma once

#include <string_view>

namespace nxmac::rewriter::internal {

std::string_view BuiltInPatchesText();

}  // namespace nxmac::rewriter::internal
