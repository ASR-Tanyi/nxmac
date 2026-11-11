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
