# nxmac::shader: the shader split (nxnative's dm/gfx/tools/shader_split.py): every NVN shader of a game's RomFS and
# main, into the package and tables nxnative's shader build and host read, byte for byte as shader_split.py writes
# them. Portable C++ on zlib (and nxmac::fs for SHA-256 and RomFS images, nxmac::rewriter for code modules): it builds
# and is tested on any platform.

# The profile table goes into the library as a string: profiles.tsv is read when CMake configures (a change to it
# configures again), split into pieces a compiler takes as string literals
set(_nxmac_profiles "${CMAKE_CURRENT_SOURCE_DIR}/profiles.tsv")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nxmac_profiles}")
file(READ "${_nxmac_profiles}" _nxmac_profiles_text)
string(LENGTH "${_nxmac_profiles_text}" _nxmac_profiles_length)
set(NXMAC_PROFILES_LITERAL "")
set(_nxmac_at 0)
while(_nxmac_at LESS _nxmac_profiles_length)
  string(SUBSTRING "${_nxmac_profiles_text}" ${_nxmac_at} 8192 _nxmac_piece)
  string(APPEND NXMAC_PROFILES_LITERAL "        R\"nxmacprofile(${_nxmac_piece})nxmacprofile\"\n")
  math(EXPR _nxmac_at "${_nxmac_at} + 8192")
endwhile()
if(NXMAC_PROFILES_LITERAL STREQUAL "")
  set(NXMAC_PROFILES_LITERAL "        \"\"\n")
endif()
string(REGEX REPLACE "\n$" "" NXMAC_PROFILES_LITERAL "${NXMAC_PROFILES_LITERAL}")
configure_file(builtin_profiles.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/builtin_profiles.cpp" @ONLY)

add_library(nxmac_shader STATIC
  archive.cpp
  files.cpp
  nvn.cpp
  profiles.cpp
  py.cpp
  readers.cpp
  scan.cpp
  split.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/builtin_profiles.cpp")
add_library(nxmac::shader ALIAS nxmac_shader)
target_include_directories(nxmac_shader
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nxmac_shader
  PUBLIC absl::status absl::statusor
  PRIVATE nxmac::fs nxmac::rewriter ZLIB::ZLIB Threads::Threads absl::strings absl::str_format)
nxmac_target_defaults(nxmac_shader)
# The generated source is not nxmac's to lint
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/builtin_profiles.cpp" PROPERTIES SKIP_LINTING ON)
