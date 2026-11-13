# nxhost: the Mac host (nxnative's dmhost). It loads the rewritten modules, answers Horizon's SVCs and the SDK's services, and draws through
# nxmac::gfx. It is built for one game: its profile's constants come from nxmac::game_profile (NXMAC_GAME). The SVC gate and stub tables
# (nxnative dm/host/entry.S) come over as .S sources; ASM is enabled for them.
add_subdirectory(third_party)   # libvpx and dav1d, from nxnative's copies (third-party code it vendors)

# The takeover files (hle_*.cpp) are listed here: each one's entries reach the tables by being linked into nxhost
add_executable(nxhost
  hle_nvdrv.cpp
  hle_tables.cpp
  log.cpp
  main.cpp)
target_link_libraries(nxhost PRIVATE nxmac::core nxmac::gfx nxmac::loader nxmac::game_profile absl::str_format absl::strings)
nxmac_target_defaults(nxhost)
