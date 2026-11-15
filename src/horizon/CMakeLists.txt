# nxmac::horizon: nxhost's stand-ins for Horizon's kernel that need no Apple API, so they build and are tested on any
# platform. So far: the guest clock and the guest's entropy (nxnative's dm/host/hle_os.c), the guest's address space
# (hle_svc.c), and the handles and result codes of the guest's kernel objects (hle_thread.c).
add_library(nxmac_horizon STATIC
  address_space.cpp
  clock.cpp
  entropy.cpp
  handles.cpp)
add_library(nxmac::horizon ALIAS nxmac_horizon)
target_include_directories(nxmac_horizon PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(nxmac_horizon PUBLIC absl::status absl::statusor PRIVATE absl::str_format Threads::Threads)
nxmac_target_defaults(nxmac_horizon)
