# Third-party libraries, pinned to release archives (whose hashes do not change) and built in the tree with nxmac's compiler, standard and
# sanitizers. Their headers are SYSTEM includes, so nxmac's warnings and clang-tidy do not report on them, and only what nxmac links is built.
# -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=OPT_IN takes installed packages (Homebrew's abseil and googletest) instead, when found.

# The system's own: threads, and zlib (the macOS SDK has it; zlib1g-dev on Debian and Ubuntu) for compressed NACP names
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER CACHE STRING "NEVER: the pinned archives; OPT_IN: installed packages when found")
include(FetchContent)

# Abseil LTS 2026-08-17
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_BUILD_TESTING OFF)
set(ABSL_ENABLE_INSTALL OFF)
FetchContent_Declare(absl
  URL https://github.com/abseil/abseil-cpp/releases/download/20260817.0/abseil-cpp-20260817.0.tar.gz
  URL_HASH SHA256=f7e05179df39c45434cad433f5783840bb3788ef322976f9138bc6b72b3a107d
  EXCLUDE_FROM_ALL
  SYSTEM
  FIND_PACKAGE_ARGS CONFIG)
FetchContent_MakeAvailable(absl)

# Mbed TLS 3.6.7 (LTS): AES-XTS/CTR/ECB and SHA-256 for the Switch formats (nxmac::fs). Only its crypto library is built, with the extra config
# cmake/mbedtls_user_config.h (SHA-256 on the ARMv8 instructions). Cache entries, not plain variables: its CMake (3.5 policies) would drop those.
set(ENABLE_PROGRAMS OFF CACHE BOOL "Mbed TLS: build its programs")
set(ENABLE_TESTING OFF CACHE BOOL "Mbed TLS: build its tests")
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "Mbed TLS: warnings as errors (off: other compilers and sanitizers warn)")
set(GEN_FILES OFF CACHE BOOL "Mbed TLS: regenerate its generated sources (the release archive has them)")
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "Mbed TLS: static libraries")
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "Mbed TLS: shared libraries")
set(MBEDTLS_USER_CONFIG_FILE "${PROJECT_SOURCE_DIR}/cmake/mbedtls_user_config.h" CACHE FILEPATH "Mbed TLS: nxmac's additions to its config")
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
  URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
  EXCLUDE_FROM_ALL
  SYSTEM
  FIND_PACKAGE_ARGS NAMES MbedTLS CONFIG)
FetchContent_MakeAvailable(mbedtls)

# Capstone 5.0.9, AArch64 only: the disassembler nxnative's rewriter reads code with (capstone==5.0.9 in its .github/requirements). The rewriter's
# rules read its instruction text and register lists, so nxmac::rewriter takes the same version and no installed package stands in for it: another
# version may print or list an instruction differently, and the rewrite would differ. Pinned by the commit of its 5.0.9 tag (Capstone publishes no
# release archive of its own), fetched alone. On macOS its CMake builds both architectures unless told not to; nxmac is arm64 only.
set(CAPSTONE_ARCHITECTURE_DEFAULT OFF CACHE BOOL "Capstone: build every architecture")
set(CAPSTONE_ARM64_SUPPORT ON CACHE BOOL "Capstone: AArch64")
set(CAPSTONE_BUILD_MACOS_THIN ON CACHE BOOL "Capstone: no universal build on macOS")
set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "Capstone: build its tests")
set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "Capstone: build cstool")
set(CAPSTONE_INSTALL OFF CACHE BOOL "Capstone: install target")
FetchContent_Declare(capstone
  GIT_REPOSITORY https://github.com/capstone-engine/capstone.git
  GIT_TAG 022575848782a4801fd150fdbc927effcbca0864   # 5.0.9
  GIT_SHALLOW ON
  EXCLUDE_FROM_ALL
  SYSTEM)
FetchContent_MakeAvailable(capstone)

# PCRE2 10.47 (8-bit, with its JIT): the regular expressions of the shader build (nxmac::dmsh). nxnative's dmsh_build.py rewrites Metal source with
# Python's re; PCRE2 matches as it does (Perl's rules: leftmost, alternatives in order, backtracking), which std::regex and RE2 do not. Pinned by the
# commit of its release tag (its JIT, sljit, is a submodule that comes with it).
set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "PCRE2: the 8-bit library")
set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "PCRE2: the 16-bit library")
set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "PCRE2: the 32-bit library")
set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "PCRE2: build pcre2grep")
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "PCRE2: build its tests")
set(PCRE2_SUPPORT_JIT ON CACHE BOOL "PCRE2: just-in-time compiling")
set(PCRE2_SUPPORT_LIBBZ2 OFF CACHE BOOL "PCRE2: bzip2 in pcre2grep")
set(PCRE2_SUPPORT_LIBZ OFF CACHE BOOL "PCRE2: zlib in pcre2grep")
set(PCRE2_SUPPORT_LIBEDIT OFF CACHE BOOL "PCRE2: libedit in pcre2test")
set(PCRE2_SUPPORT_LIBREADLINE OFF CACHE BOOL "PCRE2: readline in pcre2test")
set(PCRE2_STATIC_PIC OFF CACHE BOOL "PCRE2: position-independent static library")
set(BUILD_STATIC_LIBS ON CACHE BOOL "PCRE2: static libraries")
FetchContent_Declare(pcre2
  GIT_REPOSITORY https://github.com/PCRE2Project/pcre2.git
  GIT_TAG f454e231fe5006dd7ff8f4693fd2b8eb94333429   # pcre2-10.47
  GIT_SHALLOW ON
  EXCLUDE_FROM_ALL
  SYSTEM)
FetchContent_MakeAvailable(pcre2)

# SPIRV-Cross at the commit nxnative's shader chain pins (dm/gfx/tools/spirv_cross_bin/build.sh: vulkan-sdk-1.4.363.0), linked into nxmac::dmsh, which
# runs its command line's own code in-process (src/dmsh/cross.cpp), so the Metal source is what nxnative's spirv-cross binary writes. No installed
# package stands in for it: another version writes other source.
set(SPIRV_CROSS_CLI OFF CACHE BOOL "SPIRV-Cross: build its command-line program")
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "SPIRV-Cross: build its tests")
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "SPIRV-Cross: the C API")
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "SPIRV-Cross: no install targets")
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "SPIRV-Cross: shared library")
set(SPIRV_CROSS_STATIC ON CACHE BOOL "SPIRV-Cross: static libraries")
FetchContent_Declare(spirv_cross
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
  GIT_TAG f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7   # vulkan-sdk-1.4.363.0
  GIT_SHALLOW ON
  EXCLUDE_FROM_ALL
  SYSTEM)
FetchContent_MakeAvailable(spirv_cross)

# GoogleTest 1.18.0, with GoogleMock
if(NXMAC_BUILD_TESTS)
  set(INSTALL_GTEST OFF)
  set(BUILD_GMOCK ON)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    EXCLUDE_FROM_ALL
    SYSTEM
    FIND_PACKAGE_ARGS NAMES GTest CONFIG)
  FetchContent_MakeAvailable(googletest)
endif()
