# Unit tests (GoogleTest and GoogleMock), one executable per module, each test registered with CTest: ctest --preset <preset>.
include(GoogleTest)

#   nxmac_add_test(<name> SOURCES ... [LIBS ...])
function(nxmac_add_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;LIBS")
  add_executable(${name} ${arg_SOURCES})
  target_link_libraries(${name} PRIVATE ${arg_LIBS} GTest::gmock GTest::gtest_main)
  nxmac_target_defaults(${name})
  # PRE_TEST: the tests are listed when ctest runs, not at build time, so a build never runs a test binary
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

nxmac_add_test(core_test
  SOURCES core/title_id_test.cpp
  LIBS nxmac::core absl::strings)

# Builders of the Switch's formats with made-up keys (no game data, no console keys), for the fs and tool tests
add_library(nxmac_test_builders STATIC fs/test_builders.cpp)
target_include_directories(nxmac_test_builders PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/fs")
target_link_libraries(nxmac_test_builders PUBLIC nxmac::fs GTest::gtest PRIVATE absl::strings absl::str_format)
nxmac_target_defaults(nxmac_test_builders)

# nxmac::fs on formats the test builders make
nxmac_add_test(fs_test
  SOURCES fs/crypto_test.cpp fs/keys_test.cpp fs/nacp_test.cpp fs/nca_test.cpp fs/package_test.cpp
          fs/partition_fs_test.cpp fs/patch_test.cpp fs/romfs_image_test.cpp fs/romfs_test.cpp
  LIBS nxmac::fs nxmac_test_builders ZLIB::ZLIB absl::strings absl::str_format)
target_include_directories(fs_test PRIVATE "${PROJECT_SOURCE_DIR}/src/fs")   # the bucket trees' internals (patch_test)

# nxmac::fs on a game of yours, opt-in (skipped unless NXMAC_TEST_KEYS and NXMAC_TEST_BASE are set: fs/real_game_test.cpp)
nxmac_add_test(fs_real_test
  SOURCES fs/real_game_test.cpp
  LIBS nxmac::fs absl::strings absl::str_format)

# The nsp tool, its commands run in-process on packages the test builders make
nxmac_add_test(nsp_test
  SOURCES tools/nsp_options_test.cpp tools/nsp_test.cpp
  LIBS nxmac_nsp nxmac_test_builders)
