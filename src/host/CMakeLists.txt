# nxhost: the Mac host (nxnative's dmhost). It loads the rewritten modules, answers Horizon's SVCs and the SDK's services, and draws through
# nxmac::gfx. It is built for one game: its profile's constants come from nxmac::game_profile (NXMAC_GAME). The SVC gate (nxnative
# dm/host/entry.S) is assembly, gate.S; ASM is enabled for it.
add_subdirectory(third_party)   # libvpx and dav1d, from nxnative's copies (third-party code it vendors)

# The host's runtime, which the Mac-only tests run too: its log, its thread keys, the host gate (gate.S: the SVC gate's
# assembly, nxnative's entry.S), the loaded modules and the guest threads' environment
add_library(nxmac_host_runtime STATIC
  gate.S
  gate.cpp
  keys.cpp
  log.cpp
  modules.cpp
  thread.cpp)
add_library(nxmac::host_runtime ALIAS nxmac_host_runtime)
target_include_directories(nxmac_host_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nxmac_host_runtime PUBLIC nxmac::loader absl::str_format PRIVATE absl::strings)
nxmac_target_defaults(nxmac_host_runtime)

# The takeover files (hle_*.cpp) are listed here: each one's entries reach the tables by being linked into nxhost
add_executable(nxhost
  boot.cpp
  hle_nvdrv.cpp
  hle_tables.cpp
  main.cpp
  svc.cpp
  svc_thread.cpp)
target_link_libraries(nxhost PRIVATE nxmac::host_runtime nxmac::core nxmac::gfx nxmac::horizon nxmac::loader
  nxmac::game_profile absl::str_format absl::strings absl::synchronization)
nxmac_target_defaults(nxhost)
