# nxmac::core: the portable C++ that the host and the tools share (binary formats, profile tables, title IDs). No Apple APIs here, so it builds
# and is tested on any platform.
configure_file(version.h.in "${PROJECT_BINARY_DIR}/gen/include/nxmac/core/version.h" @ONLY)

add_library(nxmac_core STATIC
  title_id.cpp)
add_library(nxmac::core ALIAS nxmac_core)
target_include_directories(nxmac_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${PROJECT_BINARY_DIR}/gen/include")
target_link_libraries(nxmac_core
  PUBLIC absl::status absl::statusor
  PRIVATE absl::strings absl::str_format)
nxmac_target_defaults(nxmac_core)
