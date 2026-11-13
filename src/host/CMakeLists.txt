# nxhost: the Mac host (nxnative's dmhost). It loads the rewritten modules, answers Horizon's SVCs and the SDK's services, and draws through
# nxmac::gfx. It is built for one game: its profile's constants come from nxmac::game_profile (NXMAC_GAME). The SVC gate and stub tables
# (nxnative dm/host/entry.S) come over as .S sources; ASM is enabled for them.
add_subdirectory(third_party)   # libvpx and dav1d, from nxnative's copies (third-party code it vendors)

add_executable(nxhost
  log.cpp
  main.cpp)
target_link_libraries(nxhost PRIVATE nxmac::core nxmac::gfx nxmac::loader nxmac::game_profile absl::flat_hash_set absl::str_format
  absl::strings)
nxmac_target_defaults(nxhost)
