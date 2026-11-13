# Third-party code the host links, built from nxnative's copies (NXNATIVE_DIR/dm/third_party) as nxnative's dm/host/Makefile builds them: the same
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

# dav1d 1.5.4: the AV1 decoder, where VideoToolbox has none (nxnative dm/host/hle_movie_av1.c). The list meson builds for arm64: src/*.c but
# the *_tmpl.c templates (built through the per-bit-depth wrappers in config/tmpl), src/arm/cpu.c, and the NEON assembly in src/arm/64 but the
# files other assembly files include (*_tmpl.S, util.S). The host includes dav1d/*.h from include/.
set(_nxmac_dav1d "${_nxmac_tp}/dav1d")
if(NOT EXISTS "${_nxmac_dav1d}/config/config.h")
  message(FATAL_ERROR "The host needs nxnative's dav1d, which is not under NXNATIVE_DIR=${NXNATIVE_DIR} (dm/third_party/dav1d)")
endif()
file(GLOB _nxmac_dav1d_src CONFIGURE_DEPENDS
  "${_nxmac_dav1d}/src/*.c" "${_nxmac_dav1d}/config/tmpl/*.c" "${_nxmac_dav1d}/src/arm/64/*.S")
list(FILTER _nxmac_dav1d_src EXCLUDE REGEX "(_tmpl\\.(c|S)|/util\\.S)$")
list(APPEND _nxmac_dav1d_src "${_nxmac_dav1d}/src/arm/cpu.c")
list(SORT _nxmac_dav1d_src)
add_library(nxmac_dav1d STATIC ${_nxmac_dav1d_src})
add_library(nxmac::dav1d ALIAS nxmac_dav1d)
target_include_directories(nxmac_dav1d
  PUBLIC "${_nxmac_dav1d}/include"
  PRIVATE "${_nxmac_dav1d}" "${_nxmac_dav1d}/config")
# The Makefile's flags, on the C and the assembly alike
target_compile_options(nxmac_dav1d PRIVATE -O2 -fvisibility=hidden -fomit-frame-pointer -fno-align-functions)
unset(_nxmac_dav1d)
unset(_nxmac_dav1d_src)

unset(_nxmac_tp)
