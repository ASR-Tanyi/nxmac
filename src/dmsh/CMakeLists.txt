# nxmac::dmsh: the shader build (nxnative's dm/gfx/tools/dmsh_build.py and the modules it runs a shader through): each
# Switch shader of a split package becomes Metal source and its reflection, as dmsh_build.py writes them, then a Metal
# library. nxnative's translator (dmsh_xlate) and Apple's Metal compiler are run as programs; SPIRV-Cross is linked in at
# the commit nxnative pins; the regular expressions of the rewrites are matched by PCRE2. Portable C++: everything but the
# Metal compile builds and is tested on any platform.
# dmsh_pull.metal (copied from nxnative as pull.metal) goes into the library as a string, read when CMake configures
set(_nxmac_pull "${CMAKE_CURRENT_SOURCE_DIR}/pull.metal")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nxmac_pull}")
file(READ "${_nxmac_pull}" NXMAC_PULL_METAL)
configure_file(pull_metal.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/pull_metal.cpp" @ONLY)

add_library(nxmac_dmsh STATIC
  build.cpp
  cross.cpp
  entry.cpp
  fetch.cpp
  guard.cpp
  json.cpp
  metal.cpp
  misc.cpp
  process.cpp
  py.cpp
  re.cpp
  reflect.cpp
  rewrite.cpp
  side.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/pull_metal.cpp")
add_library(nxmac::dmsh ALIAS nxmac_dmsh)
target_include_directories(nxmac_dmsh
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nxmac_dmsh
  PUBLIC absl::status absl::statusor
  PRIVATE nxmac::fs pcre2-8-static spirv-cross-msl spirv-cross-hlsl spirv-cross-cpp spirv-cross-reflect spirv-cross-util
          spirv-cross-glsl spirv-cross-core Threads::Threads absl::strings absl::str_format absl::time)
target_compile_definitions(nxmac_dmsh PRIVATE PCRE2_STATIC)
nxmac_target_defaults(nxmac_dmsh)
# SPIRV-Cross's main.cpp is compiled inside cross.cpp: its include path, and none of nxmac's warnings or lint for it
set_source_files_properties(cross.cpp PROPERTIES
  INCLUDE_DIRECTORIES "${spirv_cross_SOURCE_DIR}"
  COMPILE_OPTIONS "-w"
  SKIP_LINTING ON)
