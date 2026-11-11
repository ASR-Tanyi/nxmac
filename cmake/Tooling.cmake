# clang-tidy as nxmac's own sources compile (NXMAC_CLANG_TIDY, the tidy preset) and the clang-format targets format and format-check.
# Xcode ships neither tool: brew install llvm. They are found in Homebrew's llvm without putting it on PATH.
set(_nxmac_llvm_hints /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)

option(NXMAC_CLANG_TIDY "Run clang-tidy (.clang-tidy) on nxmac's own sources as they compile" OFF)
if(NXMAC_CLANG_TIDY)
  find_program(NXMAC_CLANG_TIDY_EXE NAMES clang-tidy HINTS ${_nxmac_llvm_hints} REQUIRED)
  # The compile lines are Apple clang's, which knows warnings a Homebrew clang-tidy may not
  set(NXMAC_CLANG_TIDY_COMMAND "${NXMAC_CLANG_TIDY_EXE}" "--extra-arg=-Wno-unknown-warning-option")
  if(NXMAC_WARNINGS_AS_ERRORS)
    list(APPEND NXMAC_CLANG_TIDY_COMMAND "--warnings-as-errors=*")
  endif()
endif()

find_program(NXMAC_CLANG_FORMAT_EXE NAMES clang-format HINTS ${_nxmac_llvm_hints})
if(NXMAC_CLANG_FORMAT_EXE)
  file(GLOB_RECURSE _nxmac_format_files CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.h" "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/src/*.mm"
    "${PROJECT_SOURCE_DIR}/tests/*.h" "${PROJECT_SOURCE_DIR}/tests/*.cpp" "${PROJECT_SOURCE_DIR}/tests/*.mm")
  add_custom_target(format
    COMMAND "${NXMAC_CLANG_FORMAT_EXE}" -i ${_nxmac_format_files}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "clang-format: formatting nxmac's sources"
    VERBATIM)
  add_custom_target(format-check
    COMMAND "${NXMAC_CLANG_FORMAT_EXE}" --dry-run --Werror ${_nxmac_format_files}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "clang-format: checking nxmac's sources"
    VERBATIM)
  unset(_nxmac_format_files)
endif()
unset(_nxmac_llvm_hints)
