# Command-line tools: native C++ replacements for nxnative's Python tools (dm/tools, dm/gfx/tools), each a thin main over nxmac::core.
#   nxmac_add_tool(nx_titleid SOURCES nx_titleid.cpp [LIBS ...])   ->  build/<preset>/bin/nx_titleid
function(nxmac_add_tool name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;LIBS")
  add_executable(${name} ${arg_SOURCES})
  target_link_libraries(${name} PRIVATE nxmac::core ${arg_LIBS})
  nxmac_target_defaults(${name})
endfunction()

# nsp: the Switch packages of your games, read and checked (nxnative's nx_extract, rewritten): info, verify, extract,
# dlc. Its commands are a library, so that the tests run them in-process (tests/tools/nsp_test.cpp).
add_library(nxmac_nsp STATIC
  nsp/commands.cpp
  nsp/copy.cpp
  nsp/extract.cpp
  nsp/info.cpp
  nsp/options.cpp
  nsp/verify.cpp)
target_include_directories(nxmac_nsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nxmac_nsp
  PUBLIC nxmac::fs
  PRIVATE absl::flat_hash_set absl::strings absl::str_format Threads::Threads)
nxmac_target_defaults(nxmac_nsp)
nxmac_add_tool(nsp SOURCES nsp/main.cpp LIBS nxmac_nsp)

# nso: Switch code modules rewritten for nxnative's host (nxnative's patch_nso.py, rewritten): rewrite, info. Its
# commands are a library, so that the tests run them in-process (tests/tools/nso_test.cpp).
add_library(nxmac_nso STATIC
  nso/commands.cpp
  nso/options.cpp)
target_include_directories(nxmac_nso PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nxmac_nso
  PUBLIC nxmac::rewriter
  PRIVATE absl::strings absl::str_format Threads::Threads)
nxmac_target_defaults(nxmac_nso)
nxmac_add_tool(nso SOURCES nso/main.cpp LIBS nxmac_nso)
