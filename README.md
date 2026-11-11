# nxmac

A C++20 rewrite of [nxnative](../nxnative)'s Mac host (`dm/host`, `dm/gfx`) and, after it, its tools (`dm/tools`). It keeps using nxnative's game profiles (`dm/games/<title ID>`); the code is rewritten here.

## Requirements

- An Apple silicon Mac with macOS 26.6 or later and Xcode 27.2, as for nxnative.
- `brew install cmake ninja`, and `brew install llvm` for clang-tidy and clang-format (Xcode ships neither; CMake finds them in Homebrew's llvm without a PATH change).
- Python 3.12 or newer (as nxnative), for its `dm/games/game.py`, which writes the game profile header.
- Network on the first configure of each build folder: Abseil and GoogleTest are downloaded, pinned and hash-checked.

## Build

```sh
cmake --preset debug -DNXMAC_GAME=01007EF00011E000   # the host is built for one game, as nxnative's make GAME=
cmake --build --preset debug
ctest --preset debug
build/debug/bin/nxhost
```

`NXMAC_GAME` stays in the build folder's cache, so it is given once per preset. Without it, everything but the host is built. `cmake --workflow --preset debug` configures, builds and tests in one step.

| Preset | What it builds |
|---|---|
| `debug` | Debug |
| `release` | Optimized, with debug info for crash reports (RelWithDebInfo) |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer, dependencies included |
| `tsan` | ThreadSanitizer, dependencies included (Abseil's locks are seen) |
| `tidy` | Debug with clang-tidy on every nxmac source; warnings and findings fail the build |

Each preset builds in `build/<preset>/`, with the executables in `build/<preset>/bin/`. Formatting: `cmake --build --preset debug --target format` (or `format-check`).

## Options

| Option | Default | |
|---|---|---|
| `NXMAC_GAME` | – | Base game title ID the host is built for: a profile in nxnative's `dm/games` (or in `DM_GAMES_DIR`) |
| `NXNATIVE_DIR` | `../nxnative` | The nxnative checkout the profiles come from |
| `NXMAC_SANITIZE` | – | `address,undefined` or `thread` (the `asan` and `tsan` presets) |
| `NXMAC_CLANG_TIDY` | `OFF` | clang-tidy while compiling (the `tidy` preset) |
| `NXMAC_WARNINGS_AS_ERRORS` | `OFF` | Warnings and clang-tidy findings in nxmac's own code fail the build |
| `NXMAC_BUILD_HOST`, `NXMAC_BUILD_TOOLS`, `NXMAC_BUILD_TESTS` | `ON` (host: macOS only) | Parts of the tree to build |
| `FETCHCONTENT_TRY_FIND_PACKAGE_MODE` | `NEVER` | `OPT_IN` takes installed Abseil and GoogleTest (Homebrew's) instead of the pinned downloads |

## Layout

```
CMakeLists.txt, CMakePresets.json
cmake/            Sanitizers, Dependencies (Abseil, GoogleTest), Tooling (clang-tidy, clang-format), NxmacTargets, GameProfile
sanitizers/       ignorelist.txt (ASan/UBSan), tsan.supp (TSan)
src/core/         nxmac::core: portable C++ the host and the tools share; no Apple APIs, builds and tests on any platform
src/gfx/          nxmac::gfx: NVN / Vulkan / GL on Metal; Objective-C++ (.mm) inside, plain C++ headers outside
src/host/         nxhost (nxnative's dmhost): loader, SVC gate (.S), services; built for NXMAC_GAME
src/tools/        native replacements for nxnative's Python tools: nxmac_add_tool(<name> SOURCES ...)
tests/            GoogleTest per module: nxmac_add_test(<name> SOURCES ... LIBS ...)
```

Where new code goes: anything without Apple APIs (formats, tables, the rewriter's logic) into `src/core`, with its tests in `tests/`; Metal and Objective-C++ into `src/gfx`; the host's own code into `src/host`. Every target calls `nxmac_target_defaults()` (`cmake/NxmacTargets.cmake`) for its warnings, ARC and clang-tidy. `src/core/title_id.*` is a first, small piece that proves the chain (Abseil, tests, sanitizers); it can stay or go.
