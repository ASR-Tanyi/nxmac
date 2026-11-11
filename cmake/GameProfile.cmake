# The game the host is built for, as nxnative builds dmhost (make GAME=<id>): one build folder per game. Its profile's constants are generated
# into gen/game/game_profile.h by nxnative's dm/games/game.py, from dm/games/<id>/game.tsv or a profile in DM_GAMES_DIR.
# Targets that need them link nxmac::game_profile, which exists only when NXMAC_GAME is set.
cmake_path(SET _nxmac_nxnative_default NORMALIZE "${PROJECT_SOURCE_DIR}/../nxnative")
set(NXMAC_GAME "" CACHE STRING "Base game title ID the host is built for (a profile in nxnative's dm/games, or in DM_GAMES_DIR)")
set(NXNATIVE_DIR "${_nxmac_nxnative_default}" CACHE PATH "The nxnative checkout whose game profiles (dm/games) the host is built for")
unset(_nxmac_nxnative_default)

if(NXMAC_GAME)
  # nxnative's Python minimum; it also keeps CMake from taking the Command Line Tools' Python 3.9 on macOS
  find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
  set(_nxmac_game_py "${NXNATIVE_DIR}/dm/games/game.py")
  if(NOT EXISTS "${_nxmac_game_py}")
    message(FATAL_ERROR "NXMAC_GAME needs nxnative's dm/games/game.py, which is not under NXNATIVE_DIR=${NXNATIVE_DIR}")
  endif()
  # A title ID without a profile stops here, with game.py's list of the profiles there are
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${_nxmac_game_py}" dir "${NXMAC_GAME}"
    OUTPUT_VARIABLE NXMAC_GAME_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _nxmac_game_err
    RESULT_VARIABLE _nxmac_game_rc)
  if(NOT _nxmac_game_rc EQUAL 0)
    message(FATAL_ERROR "NXMAC_GAME=${NXMAC_GAME}: ${_nxmac_game_err}")
  endif()
  message(STATUS "nxmac: game profile ${NXMAC_GAME} (${NXMAC_GAME_DIR})")

  # Run on every build, because the profile can change under the same title ID (a DM_GAMES_DIR one, an edited game.tsv). game.py rewrites the
  # header only when its text changes, so an unchanged profile rebuilds nothing (Ninja restats byproducts).
  set(_nxmac_game_h "${PROJECT_BINARY_DIR}/gen/game/game_profile.h")
  add_custom_target(nxmac_game_profile_header
    COMMAND "${Python3_EXECUTABLE}" "${_nxmac_game_py}" header "${NXMAC_GAME}" "${_nxmac_game_h}"
    BYPRODUCTS "${_nxmac_game_h}"
    COMMENT "game_profile.h for ${NXMAC_GAME}"
    VERBATIM)
  add_library(nxmac_game_profile INTERFACE)
  add_library(nxmac::game_profile ALIAS nxmac_game_profile)
  target_include_directories(nxmac_game_profile INTERFACE "${PROJECT_BINARY_DIR}/gen/game")
  add_dependencies(nxmac_game_profile nxmac_game_profile_header)   # followed by every target that links it
  unset(_nxmac_game_py)
  unset(_nxmac_game_h)
endif()
