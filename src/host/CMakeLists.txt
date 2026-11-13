# nxhost: the Mac host (nxnative's dmhost). It loads the rewritten modules, answers Horizon's SVCs and the SDK's services, and draws through
# nxmac::gfx. It is built for one game: its profile's constants come from nxmac::game_profile (NXMAC_GAME). The SVC gate and stub tables
# (nxnative dm/host/entry.S) come over as .S sources; ASM is enabled for them.
add_subdirectory(third_party)   # nxnative's copy of libvpx, built as its Makefile builds it

add_executable(nxhost
  main.cpp)
target_link_libraries(nxhost PRIVATE nxmac::core nxmac::gfx nxmac::game_profile absl::str_format)
nxmac_target_defaults(nxhost)
