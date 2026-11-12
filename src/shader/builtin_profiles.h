// The profile table built into the library: the text of src/shader/profiles.tsv (internal to nxmac::shader; the build
// writes the definition, see CMakeLists.txt).
#pragma once

#include <string_view>

namespace nxmac::shader::internal {

std::string_view BuiltInProfilesText();

}  // namespace nxmac::shader::internal
