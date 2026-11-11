# nxmac

A C++20 rewrite of [nxnative](../nxnative)'s Mac host (`dm/host`, `dm/gfx`) and, after it, its tools (`dm/tools`). It keeps using nxnative's game profiles (`dm/games/<title ID>`); the code is rewritten here.

## Requirements

- An Apple silicon Mac with macOS 26.6 or later and Xcode 27.2, as for nxnative.
- `brew install cmake ninja`, and `brew install llvm` for clang-tidy and clang-format (Xcode ships neither; CMake finds them in Homebrew's llvm without a PATH change).
- Python 3.12 or newer (as nxnative), for its `dm/games/game.py`, which writes the game profile header.
- Network on the first configure of each build folder: Abseil, Mbed TLS and GoogleTest are downloaded, pinned and hash-checked.

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
| `FETCHCONTENT_TRY_FIND_PACKAGE_MODE` | `NEVER` | `OPT_IN` takes installed Abseil, Mbed TLS and GoogleTest (Homebrew's) instead of the pinned downloads |

## Layout

```
CMakeLists.txt, CMakePresets.json
cmake/            Sanitizers, Dependencies (Abseil, Mbed TLS, GoogleTest), Tooling (clang-tidy, clang-format), NxmacTargets, GameProfile
sanitizers/       ignorelist.txt (ASan/UBSan), tsan.supp (TSan)
src/core/         nxmac::core: portable C++ the host and the tools share; no Apple APIs, builds and tests on any platform
src/fs/           nxmac::fs: the Switch's content formats, read and hash-checked (what nx_extract did with LibHac); portable
src/gfx/          nxmac::gfx: NVN / Vulkan / GL on Metal; Objective-C++ (.mm) inside, plain C++ headers outside
src/host/         nxhost (nxnative's dmhost): loader, SVC gate (.S), services; built for NXMAC_GAME
src/tools/        native replacements for nxnative's tools: nxmac_add_tool(<name> SOURCES ...); nsp/ is the first
tests/            GoogleTest per module: nxmac_add_test(<name> SOURCES ... LIBS ...)
```

## nsp

`build/<preset>/bin/nsp` reads the Switch packages of your games and checks every byte it reads against their NCAs' hash trees, on as many threads as the Mac has CPUs (`-j` to change). It replaces nxnative's `nx_extract`, with a command line of its own. With BotW's files beside `nxmac` (`nsp help` for everything):

```sh
nsp=build/release/bin/nsp
keys=../keys/prod.keys                        # or put it at ~/.switch/prod.keys and leave -k out
game="../nsp/The Legend of Zelda Breath of the Wild [01007EF00011E000][v0](eShop).nsp"
update="../nsp/The Legend of Zelda Breath of the Wild [01007EF00011E800][v1114112][US][Update v1.9.0p].nsp"

$nsp info -k $keys "$game" "$update" ../nsp/*DLC*.nsp            # what they hold; whether the keys open them
$nsp verify -k $keys "$game" "$update" ../nsp/*DLC*.nsp          # read everything, write nothing
$nsp extract -k $keys -o ../nsp/01007EF00011E000-work/extract "$game" "$update"
$nsp dlc -k $keys -o ../nsp/01007EF00011E000-work/aoc ../nsp/*DLC*.nsp
```

`extract` writes what nxnative's scripts and host take: `ExeFS/`, `RomFS.img` (the DMROMFS1 image, written byte for byte as `nx_extract --romfs-image` writes it), `control.nacp` and `icon.jpg`; `--romfs-dir` writes a `RomFS/` folder instead of the image. `dlc` writes `<index>.img` per add-on content, as `aoc/<index>.img`. The game and its update are told apart by their content metas, in either order. A file that fails its hash check is named and not written (an image is only renamed from `.part` once all its files are in), and the exit code says what happened: 0 done, 1 an error, 2 a wrong command line, 3 damaged data, 4 missing keys.

To compare with nxnative's `nx_extract` on the same game:

```sh
w=../nsp/01007EF00011E000-work
../nxnative/dm/tools/nx_extract_bin/nx_extract $keys "$game" "$update" $w/nx_extract --romfs-image --app 01007EF00011E000
cmp $w/nx_extract/RomFS.img $w/extract/RomFS.img && diff -r $w/nx_extract/ExeFS $w/extract/ExeFS && cmp $w/nx_extract/control.nacp $w/extract/control.nacp
```

## nxmac::fs

The library nsp is built on (`src/fs`): `prod.keys` and tickets, NSP / PFS0 and HFS0, NCA2 and NCA3 (AES-XTS headers, key areas and title keys, AES-CTR sections), their hash trees (HierarchicalSha256 and IVFC: every block read is checked), update patches (BKTR: the indirect and AesCtrEx tables, laid over the base game's section), RomFS and nxnative's DMROMFS1 image of it, content metas and control data (NACP, with the compressed names of newer SDKs). Key values never appear in a message. Not read yet: sparse and compressed sections, XCI, NSZ, key derivation from master keys (`prod.keys` from Lockpick_RCM has the derived keys).

`fs_test` checks it on formats the test builders make with made-up keys. `fs_real_test` checks it on a game of yours: every byte of the ExeFS and of the RomFS (patched by the update) and the DLC's RomFS, through the hash trees. It is skipped unless the paths are set; run it from the release build:

```sh
cmake --workflow --preset release   # once; or cmake --build --preset release after changes
NXMAC_TEST_KEYS=../keys/prod.keys \
NXMAC_TEST_BASE="../nsp/The Legend of Zelda Breath of the Wild [01007EF00011E000][v0](eShop).nsp" \
NXMAC_TEST_UPDATE="../nsp/The Legend of Zelda Breath of the Wild [01007EF00011E800][v1114112][US][Update v1.9.0p].nsp" \
NXMAC_TEST_DLC="../nsp/The Legend of Zelda Breath of the Wild [DLC Pack 1][01007EF00011F001][v196608][US].nsp" \
build/release/bin/fs_real_test
```

## CI

`.github/workflows/ci.yml` runs on pull requests and on pushes to `main`:

| Job | Runner | What it checks |
|---|---|---|
| Linux debug, asan, tsan | `ubuntu-24.04`, clang 18 | core, tools and tests (the host and gfx are Mac-only) |
| Mac debug, asan, tsan | `xcode-27` (Xcode 27.2, as nxnative) | everything, the host built for `GAME`; the tests, then an `nxhost` start |
| clang-format and clang-tidy | `xcode-27`, LLVM 23.1.4 (nxnative's lint-c.yml pin) | `format-check`, then the `tidy` preset |

The Mac jobs take the game's profile from nxnative at the commit `NXNATIVE_REF` names (a sparse checkout of `dm/games/<GAME>`), so nxnative's main moving does not change CI: raise it on purpose. `xcode-27` is GitHub's Xcode 27 preview image, which actionlint does not know yet (`.github/actionlint.yaml`). CI formats with clang-format 23.1.4: if Homebrew's llvm is another version and the two disagree, use CI's (`pipx install clang-format==23.1.4`, then `-DNXMAC_CLANG_FORMAT_EXE=$(which clang-format)`).

## Adding code

Where new code goes: the Switch's content formats into `src/fs`, the commands of a tool into `src/tools/<tool>/` (a library, so that its tests run it in-process, and a `main.cpp`); anything else without Apple APIs (tables, the rewriter's logic) into `src/core`, with its tests in `tests/`; Metal and Objective-C++ into `src/gfx`; the host's own code into `src/host`. Every target calls `nxmac_target_defaults()` (`cmake/NxmacTargets.cmake`) for its warnings, ARC and clang-tidy. `src/core/title_id.*` is a first, small piece that proves the chain (Abseil, tests, sanitizers); it can stay or go.
