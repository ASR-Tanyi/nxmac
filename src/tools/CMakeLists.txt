# Command-line tools: native C++ replacements for nxnative's Python tools (dm/tools, dm/gfx/tools), each a thin main over nxmac::core.
#   nxmac_add_tool(nx_titleid SOURCES nx_titleid.cpp [LIBS ...])   ->  build/<preset>/bin/nx_titleid
function(nxmac_add_tool name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;LIBS")
  add_executable(${name} ${arg_SOURCES})
  target_link_libraries(${name} PRIVATE nxmac::core ${arg_LIBS})
  nxmac_target_defaults(${name})
endfunction()

# No tools yet: the host comes first.
