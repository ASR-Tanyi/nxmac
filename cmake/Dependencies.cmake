# Third-party libraries, pinned to release archives (whose hashes do not change) and built in the tree with nxmac's compiler, standard and
# sanitizers. Their headers are SYSTEM includes, so nxmac's warnings and clang-tidy do not report on them, and only what nxmac links is built.
# -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=OPT_IN takes installed packages (Homebrew's abseil and googletest) instead, when found.
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
