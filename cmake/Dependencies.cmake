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
