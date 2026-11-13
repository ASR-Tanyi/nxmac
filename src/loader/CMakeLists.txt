# nxmac::loader: nxhost's module loader, in the role of Horizon's rtld as nxnative's dm/host/loader.c plays it. So far: the rewritten modules'
# layout tables (<module>.fixups.tsv, which nso writes), which modules a profile loads, their images mapped (<module>.patched.bin), their
# symbols, their relocation (with the host's takeover tables and named stubs) and the host gates they hold (<module>.gates.tsv). Portable C++
# (POSIX mmap / mprotect): it builds and is tested on any platform.
add_library(nxmac_loader STATIC
  gates.cpp
  hle.cpp
  image.cpp
  layout.cpp
  modules.cpp
  relocate.cpp
  symbols.cpp)
add_library(nxmac::loader ALIAS nxmac_loader)
target_include_directories(nxmac_loader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(nxmac_loader
  PUBLIC absl::status absl::statusor
  PRIVATE absl::strings absl::str_format)
nxmac_target_defaults(nxmac_loader)
