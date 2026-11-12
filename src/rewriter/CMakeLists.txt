# nxmac::rewriter: the NSO rewriter (nxnative's dm/tools/patch_nso.py with its module loader, function matcher and
# patch tables). A Switch module becomes an image that runs natively on Apple systems; the host that loads it is
# nxnative's, so the output is the one patch_nso.py gives, byte for byte. Portable C++ on Capstone: it builds and is
# tested on any platform.

# The patch tables go into the library as a string: patches.tsv is read when CMake configures (a change to it
# configures again), split into pieces a compiler takes as string literals
set(_nxmac_patches "${CMAKE_CURRENT_SOURCE_DIR}/patches.tsv")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nxmac_patches}")
file(READ "${_nxmac_patches}" _nxmac_patches_text)
string(LENGTH "${_nxmac_patches_text}" _nxmac_patches_length)
set(NXMAC_PATCHES_LITERAL "")
set(_nxmac_at 0)
while(_nxmac_at LESS _nxmac_patches_length)
  string(SUBSTRING "${_nxmac_patches_text}" ${_nxmac_at} 8192 _nxmac_piece)
  string(APPEND NXMAC_PATCHES_LITERAL "        R\"nxmacpatch(${_nxmac_piece})nxmacpatch\"\n")
  math(EXPR _nxmac_at "${_nxmac_at} + 8192")
endwhile()
if(NXMAC_PATCHES_LITERAL STREQUAL "")
  set(NXMAC_PATCHES_LITERAL "        \"\"\n")
endif()
string(REGEX REPLACE "\n$" "" NXMAC_PATCHES_LITERAL "${NXMAC_PATCHES_LITERAL}")
configure_file(builtin_patches.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/builtin_patches.cpp" @ONLY)

add_library(nxmac_rewriter STATIC
  disasm.cpp
  emit.cpp
  error.cpp
  games/01007EF00011E000.cpp
  games/registry.cpp
  image.cpp
  jumps.cpp
  lz4.cpp
  match.cpp
  module.cpp
  patches.cpp
  rewriter.cpp
  spin.cpp
  text.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/builtin_patches.cpp")
add_library(nxmac::rewriter ALIAS nxmac_rewriter)
target_include_directories(nxmac_rewriter
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nxmac_rewriter
  PUBLIC nxmac::core absl::status absl::statusor absl::flat_hash_map
  PRIVATE nxmac::fs capstone_static absl::flat_hash_set absl::inlined_vector absl::strings absl::str_format)
nxmac_target_defaults(nxmac_rewriter)
# The generated source is not nxmac's to lint
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/builtin_patches.cpp" PROPERTIES SKIP_LINTING ON)
