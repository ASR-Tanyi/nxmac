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

# nxmac::rewriter: its readers and text matching, and the rewriter itself held to nxnative's on the fixtures of
# rewriter/fixtures (golden_test.cpp)
nxmac_add_test(rewriter_test
  SOURCES rewriter/golden_test.cpp rewriter/module_test.cpp rewriter/patches_test.cpp rewriter/text_test.cpp
  LIBS nxmac::rewriter nxmac::fs absl::strings)
target_include_directories(rewriter_test PRIVATE "${PROJECT_SOURCE_DIR}/src/rewriter")   # its internals (text.h, lz4.h)
target_compile_definitions(rewriter_test PRIVATE "NXMAC_REWRITER_FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/rewriter/fixtures\"")

# The nso tool, its commands run in-process on the same fixtures
nxmac_add_test(nso_test
  SOURCES tools/nso_options_test.cpp tools/nso_test.cpp
  LIBS nxmac_nso)
target_compile_definitions(nso_test PRIVATE "NXMAC_REWRITER_FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/rewriter/fixtures\"")

# nxmac::shader held to nxnative's shader_split.py on the made-up games of shader/fixtures, and its internals
nxmac_add_test(shader_test
  SOURCES shader/golden_test.cpp shader/profiles_test.cpp shader/py_test.cpp
  LIBS nxmac::shader nxmac::fs ZLIB::ZLIB absl::strings)
target_include_directories(shader_test PRIVATE "${PROJECT_SOURCE_DIR}/src/shader")   # its internals (py.h, archive.h, nvn.h)
target_compile_definitions(shader_test PRIVATE "NXMAC_SHADER_FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/shader/fixtures\"")

# The shader command line, in-process
nxmac_add_test(shader_tool_test
  SOURCES tools/shader_options_test.cpp tools/shader_test.cpp
  LIBS nxmac_shader_tool absl::strings)
target_compile_definitions(shader_tool_test PRIVATE "NXMAC_SHADER_FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/shader/fixtures\""
  "NXMAC_DMSH_FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/dmsh/fixtures\"")

# nxmac::dmsh: its Python-compatible layers, and its ports of dmsh_build.py's modules held to the Python functions
nxmac_add_test(dmsh_test
  SOURCES dmsh/build_test.cpp dmsh/calls_test.cpp dmsh/json_test.cpp dmsh/re_test.cpp
  LIBS nxmac::dmsh nxmac::fs ZLIB::ZLIB absl::strings)
target_include_directories(dmsh_test PRIVATE "${PROJECT_SOURCE_DIR}/src/dmsh")   # its internals
target_compile_definitions(dmsh_test PRIVATE "NXMAC_DMSH_FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/dmsh/fixtures\"")

# nxmac::loader, and that it reads the tables the rewriter writes (the rewriter's fixtures)
nxmac_add_test(loader_test
  SOURCES loader/gates_test.cpp loader/hle_test.cpp loader/image_test.cpp loader/layout_test.cpp loader/modules_test.cpp loader/relocate_test.cpp
          loader/sdk_test.cpp loader/symbols_test.cpp
  LIBS nxmac::loader nxmac::rewriter absl::strings)
target_compile_definitions(loader_test PRIVATE "NXMAC_REWRITER_FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/rewriter/fixtures\"")

# nxmac::horizon, on a clock the tests set and address spaces they reserve
nxmac_add_test(horizon_test
  SOURCES horizon/address_space_test.cpp horizon/clock_test.cpp horizon/entropy_test.cpp horizon/handles_test.cpp
  LIBS nxmac::horizon absl::strings)

# The host gate end to end (gate.S), which runs rewritten code natively: Apple's arm64 alone, where the host is built
if(TARGET nxmac_host_runtime)
  nxmac_add_test(gate_test
    SOURCES host/gate_test.cpp
    LIBS nxmac::host_runtime nxmac::loader nxmac::rewriter)
  target_compile_definitions(gate_test PRIVATE "NXMAC_REWRITER_FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/rewriter/fixtures\"")
endif()
