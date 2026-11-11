# nxmac: a C++20 rewrite of nxnative's Mac host (dm/host, dm/gfx) and, after it, its tools (dm/tools).
#
#   cmake --preset debug -DNXMAC_GAME=01007EF00011E000     (the host is built for one game, as nxnative's make GAME=)
#   cmake --build --preset debug && ctest --preset debug
#
# The portable parts (core, tools, tests) build on any platform, so they can be tested off the Mac; gfx and the host are Apple-only.
cmake_minimum_required(VERSION 3.28...4.2)

# Read when the toolchain is set up, so they come before project(). The minimum macOS is nxnative's (dm/mac/min_os.mk); arm64 only, because the
# guest's own arm64 code runs natively. An explicit SDK keeps -isysroot on every compile line (CMake 4 leaves it out), which clang-tidy needs.
set(CMAKE_OSX_DEPLOYMENT_TARGET "26.6" CACHE STRING "Minimum macOS version of everything nxmac builds")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Architectures nxmac builds for (the host runs arm64 guest code natively)")
if(CMAKE_HOST_APPLE)
  set(CMAKE_OSX_SYSROOT "macosx" CACHE STRING "SDK nxmac builds against")
endif()

project(nxmac
  VERSION 0.1.0
  DESCRIPTION "C++ rewrite of nxnative's Mac host and tools"
  LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJCXX)   # gfx and the host's Apple front end (.mm)
  enable_language(ASM)      # the host's SVC gate and stub tables (nxnative dm/host/entry.S)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# ---- options (the presets set them; the README lists them all) ----
option(NXMAC_BUILD_HOST "Build the Mac host (Apple only; needs NXMAC_GAME)" ${APPLE})
option(NXMAC_BUILD_TOOLS "Build the command-line tools" ON)
option(NXMAC_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(NXMAC_WARNINGS_AS_ERRORS "Treat warnings (and clang-tidy findings) in nxmac's own code as errors" OFF)
# Also: NXMAC_SANITIZE (cmake/Sanitizers.cmake), NXMAC_CLANG_TIDY (cmake/Tooling.cmake), NXMAC_GAME and NXNATIVE_DIR (cmake/GameProfile.cmake)

if(NXMAC_BUILD_HOST AND NOT APPLE)
  message(FATAL_ERROR "NXMAC_BUILD_HOST needs macOS: the host is built on Metal and Apple's frameworks")
endif()

# ---- language and build defaults ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)              # third-party C (nxnative's libvpx, dav1d, stb_vorbis) when it comes over
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # clangd (.clangd) and clang-tidy read build/<preset>/compile_commands.json
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)   # no C++20 modules: skips the dependency scan (and the need for clang-scan-deps)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")   # build/<preset>/bin: nxhost, the tools, the tests

get_property(_nxmac_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _nxmac_multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(Sanitizers)     # first: the dependencies are built with the same sanitizers (TSan must see every lock, Abseil's included)
include(Dependencies)
include(Tooling)
include(NxmacTargets)
include(GameProfile)

# ---- the tree ----
add_subdirectory(src/core)
add_subdirectory(src/fs)
if(APPLE)
  add_subdirectory(src/gfx)
endif()
if(NXMAC_BUILD_HOST)
  if(TARGET nxmac::game_profile)
    add_subdirectory(src/host)
  else()
    message(STATUS "nxmac: host skipped: it is built for one game, -DNXMAC_GAME=<base game title ID> "
                   "(python3 ${NXNATIVE_DIR}/dm/games/game.py list)")
  endif()
endif()
if(NXMAC_BUILD_TOOLS)
  add_subdirectory(src/tools)
endif()
if(NXMAC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "nxmac ${PROJECT_VERSION}: C++${CMAKE_CXX_STANDARD} ${CMAKE_BUILD_TYPE}, sanitize='${NXMAC_SANITIZE}', "
               "clang-tidy=${NXMAC_CLANG_TIDY}, game='${NXMAC_GAME}'")
