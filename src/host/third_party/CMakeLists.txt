# Third-party C the host links, built from nxnative's copies (NXNATIVE_DIR/dm/third_party) as nxnative's dm/host/Makefile builds them: the same
# sources, configuration headers and -O2 in every build type. Not nxmac's own code, so no nxmac_target_defaults (warnings, clang-tidy).
set(_nxmac_tp "${NXNATIVE_DIR}/dm/third_party")

# libvpx v1.17.0: the VP8 / VP9 decoders, for the games' WebM movies (nxnative dm/host/hle_movie.c). Every .c of nxnative's copy (the sources
# libvpx's own build compiles for that configuration), with its configure output in config/; the host includes vpx/*.h from the top.
set(_nxmac_vpx "${_nxmac_tp}/libvpx")
if(NOT EXISTS "${_nxmac_vpx}/config/vpx_config.h")
  message(FATAL_ERROR "The host needs nxnative's libvpx, which is not under NXNATIVE_DIR=${NXNATIVE_DIR} (dm/third_party/libvpx)")
endif()
file(GLOB_RECURSE _nxmac_vpx_src CONFIGURE_DEPENDS "${_nxmac_vpx}/*.c")
list(SORT _nxmac_vpx_src)
add_library(nxmac_vpx STATIC ${_nxmac_vpx_src})
add_library(nxmac::vpx ALIAS nxmac_vpx)
target_include_directories(nxmac_vpx
  PUBLIC "${_nxmac_vpx}"
  PRIVATE "${_nxmac_vpx}/config")
target_compile_options(nxmac_vpx PRIVATE -O2)   # after the build type's flags, so it wins in Debug too
unset(_nxmac_vpx)
unset(_nxmac_vpx_src)

unset(_nxmac_tp)
