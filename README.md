# nxmac

A C++20 rewrite of [nxnative](../nxnative)'s Mac host (`dm/host`, `dm/gfx`) and, after it, its tools (`dm/tools`). It keeps using nxnative's game profiles (`dm/games/<title ID>`); the code is rewritten here.

## Requirements

- An Apple silicon Mac with macOS 15 or later and Xcode 26 or later: what nxmac builds runs on macOS 15 (`CMAKE_OSX_DEPLOYMENT_TARGET`), and the shader libraries `shader build` compiles take macOS 15 as their minimum too. (nxnative itself asks for macOS 26.6 and Xcode 27.2.) `shader build` runs Xcode's Metal compiler, which Xcode 26 downloads as a component of its own: `xcodebuild -downloadComponent MetalToolchain` when it is missing.
- `brew install cmake ninja`, and `brew install llvm` for clang-tidy and clang-format (Xcode ships neither; CMake finds them in Homebrew's llvm without a PATH change).
- Python 3.12 or newer (as nxnative), for its `dm/games/game.py`, which writes the game profile header.
- Network on the first configure of each build folder: Abseil, Mbed TLS and GoogleTest are downloaded, pinned and hash-checked, and Capstone 5.0.9, PCRE2 10.47 and SPIRV-Cross (vulkan-sdk-1.4.363.0, the commit nxnative's `spirv_cross_bin/build.sh` pins) are fetched from their repositories at those commits (so `git` is needed).

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
docs/             dmhost-plan.md: the plan for nxhost, the dmhost rewrite (items, order, status)
cmake/            Sanitizers, Dependencies (Abseil, Mbed TLS, Capstone, GoogleTest), Tooling (clang-tidy, clang-format), NxmacTargets, GameProfile
sanitizers/       ignorelist.txt (ASan/UBSan), tsan.supp (TSan)
src/core/         nxmac::core: portable C++ the host and the tools share; no Apple APIs, builds and tests on any platform
src/fs/           nxmac::fs: the Switch's content formats, read and hash-checked (what nx_extract did with LibHac); portable
src/rewriter/     nxmac::rewriter: the NSO rewriter (nxnative's patch_nso.py), its patch tables (patches.tsv) and each game's stub code (games/); portable
src/shader/       nxmac::shader: the shader split (nxnative's shader_split.py) and its profile table (profiles.tsv); portable
src/dmsh/         nxmac::dmsh: the shader build (nxnative's dmsh_build.py and its modules), on PCRE2 and SPIRV-Cross; portable
                  but for the Metal compile, which runs Xcode's tools
src/loader/       nxmac::loader: nxhost's module loader (nxnative's dm/host/loader.c): so far the rewritten modules' layout tables,
                  which modules a profile loads, their images mapped, their symbols and their relocation;
                  portable (POSIX)
src/gfx/          nxmac::gfx: NVN / Vulkan / GL on Metal; Objective-C++ (.mm) inside, plain C++ headers outside
src/host/         nxhost (nxnative's dmhost): loader, SVC gate (.S), services; built for NXMAC_GAME
src/tools/        native replacements for nxnative's tools: nxmac_add_tool(<name> SOURCES ...); nsp/, nso/ and shader/
tools/            scripts: export_patches.py (nxnative's patch tables -> src/rewriter/patches.tsv), export_shader_profiles.py
                  (the profiles' shader values -> src/shader/profiles.tsv), export_dmsh_abi.py (nxnative's shader library
                  ABI and chain version -> src/dmsh/abi.h)
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

## nso

`build/<preset>/bin/nso` rewrites a game's code modules where Apple's kernel requires it: nxnative's `patch_nso.py`, rewritten, with a command line of its own (`nso help` for everything). Its output is what nxnative's host loads, and it is meant to be the same, byte for byte, as `patch_nso.py`'s: the host stays nxnative's for now. After `nsp extract` (above), for BotW:

```sh
nso=build/release/bin/nso
w=../nsp/01007EF00011E000-work
x=$w/extract/ExeFS
$nso info $x/main $x/subsdk0 $x/sdk                                       # build IDs, and what the patch tables hold for them
$nso rewrite -g 01007EF00011E000 -o $w/patched $x/main $x/subsdk0 $x/sdk    # the profile's modules (not rtld: the host stands in for it)
```

Each module gets `<name>.patched.bin` and its tables in the output folder (`fixups`, `gates`, `sites`, `cuts`, `obs`, `changed`, and `match` for a build the patch tables do not register), the modules rewritten side by side. The report is `patch_nso.py`'s: a line per module with the count of each kind of site, and a line that starts with `[RED]` for anything the rewrite could not do as nxnative's would. A module that cannot be rewritten (a jump table through x18 of an unknown form, a cut whose bytes differ) stops with exit code 1 and writes nothing. `-g` is nxnative's `DM_GAME`.

To check it against nxnative's rewriter on the same modules (a venv with nxnative's `.github/requirements/tools.txt`), and then run the result on nxnative's host:

```sh
for m in main subsdk0 sdk; do DM_GAME=01007EF00011E000 ../venv/bin/python3 ../nxnative/dm/tools/patch_nso.py $x/$m $w/patch_nso $m; done
for f in $w/patched/*; do cmp $f $w/patch_nso/$(basename $f) && echo "same: $(basename $f)"; done
make -C ../nxnative/dm/host GAME=01007EF00011E000 dmhost
# then start dmhost as nxnative's docs/development.md has it ("run on the Mac"), with $w/patched as its modules folder
```

## nxmac::rewriter

The library nso is built on (`src/rewriter`), a port of `patch_nso.py` and what it imports (`nso.py`, `nso_code.py`, `nso_match.py`, `nso_syms.py`, `nso_asm.py`) that keeps their order of work and their names, so the two can be read side by side:

- the module loader (NSO and NRO, checked as Horizon checks them; LZ4 segments);
- the rewrites every module gets: x18 and the thread registers become thread slots, `svc` and the timers go through the host gate, exclusive sequences that use x18 are copied whole, jump tables and tail calls through x18 are read in each of the forms compilers make, `sub sp, sp, xN` is aligned, exclusive accesses get their ordering, spin waits yield;
- the layouts: the patch area after the image, before the base (a module with a large bss), or both (a text longer than a branch reaches);
- per-build patches: registered cuts and observation gates, function-matched cuts for builds a profile does not register, the SDK's address region and StandardAllocator gates, frame-step and other stubs.

Capstone 5.0.9 decodes for it, the version nxnative pins: the rules read Capstone's instruction text and register lists, so another version could rewrite differently.

**Patch tables.** nxnative keeps what it knows of each build in Python files (`dm/tools/sdk_patches.py`, `engine_patches.py`, `dm/games/<id>/patches.py`). `tools/export_patches.py` writes their tables to `src/rewriter/patches.tsv`, which is compiled into the library; run it again when nxnative's patch files change, then build (`nso -p <file>` takes other tables). What those files hold as code, the frame-step and stub functions of a build, is ported by hand in `src/rewriter/games/<title ID>.cpp` and listed in `games/registry.cpp`. Breath of the Wild 1.9.0's is there; the five other builds with such code (the exporter lists them) stop with a message that says so, as does a profile that places its frame step by function match.

**Not ported yet.** nxnative's finders for engine code: Unreal Engine (`ue_cvar.py`, `ue_fixed_rate.py`, `ue_malloc.py`, `ue_region.py`, `ue_ring.py`), Unity (`unity_fps.py`, `unity_bucket.py`), Snowdrop (`snowdrop_pools.py`) and `fps_target.py`. A module that shows an Unreal or Unity mark gets a `[RED]` line saying its rewrite lacks what they do; Snowdrop's allocator and `fps_target.py`'s field are found by code shape, so their absence is not reported. Also not read: ZBIC (zstd) segments of system 22.0.0 modules and compressed NROs, and `build_src.sha256` is not written (it keys the NSP script's cache on nxnative's own sources).

**Tests.** `rewriter_test` holds the rewriter to nxnative's: `tests/rewriter/fixtures` has small modules laid out as an NSO is (hand-written assembly for each kind of site, GoogleTest compiled for aarch64 with x18 in use, a registered build and an "update" of it, a stand-in for BotW 1.9.0's main that holds only the instruction words its patches assert), and `expected.tsv` has the SHA-256 of every file `patch_nso.py` writes for them. `fixtures/src/make_fixtures.sh` makes both again (it needs nxnative, its pinned Python packages and a clang with the aarch64 target).

## shader

`build/<preset>/bin/shader` splits out every NVN shader of a game, from its RomFS and its main, into the package and tables nxnative's shader build (`dmsh_build.py`) and host read: nxnative's `dm/gfx/tools/shader_split.py`, rewritten, with a command line of its own (`shader help` for everything). Its output is meant to be the same, byte for byte, as `shader_split.py`'s. After `nsp extract` (above), for BotW (1.9.0's merged RomFS, as nxnative's profile has it):

```sh
shader=build/release/bin/shader
w=../nsp/01007EF00011E000-work
$shader split -g 01007EF00011E000 -o $w/shader --main $w/extract/ExeFS/main $w/extract/RomFS.img
```

`$w/shader` gets `pkg/` (one `<stage>_<group>.bin` per stage and source group, a record per shader key) and the tables `shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv` and `pso_pairs.tsv`. The RomFS is an image or a folder; several versions are given each with its own `--main`, base game first (their union, as `shader_split.py`'s comma-separated lists), and `--dlc` adds an add-on content RomFS. Files are scanned on as many threads as the Mac has CPUs (`-j`). What it prints is `shader_split.py`'s: `progress: <done>/<total>`, a `[RED]` line for each file it leaves out (one that does not read, or one in another engine's format, below), and the summary line. A stop (a key whose bytes differ between two files, an archive it would take apart whole, below) has exit code 1 and writes nothing. `-g` is nxnative's `DM_GAME`.

To check it against nxnative's `shader_split.py` on the same files (a venv with nxnative's `.github/requirements/tools.txt`), and against the tables in nxnative's profile, whose data rows the NSP script requires the split to give again:

```sh
DM_GAME=01007EF00011E000 ../venv/bin/python3 ../nxnative/dm/gfx/tools/shader_split.py $w/extract/RomFS.img $w/extract/ExeFS/main \
    $w/shader_py/pkg $w/shader_py/shader_map.tsv $w/shader_py/shader_facts.tsv $w/shader_py/shader_pairs.tsv $w/shader_py/pso_pairs.tsv
diff -r $w/shader_py $w/shader && echo same
for t in shader_map shader_facts shader_pairs pso_pairs; do
    diff <(grep -v '^#' ../nxnative/dm/games/01007EF00011E000/$t.tsv) <(grep -v '^#' $w/shader/$t.tsv) > /dev/null && echo "same rows: $t"
done
# then the Mac's shader libraries from the package, as nxnative's docs/development.md builds them:
DM_GAME=01007EF00011E000 ../venv/bin/python3 ../nxnative/dm/gfx/tools/dmsh_build.py $w/shader/pkg <shader cache folder>
```

Then the game's shader library, from that package: nxnative's `dm/gfx/tools/dmsh_build.py`, rewritten (`shader build`). Each shader goes through nxnative's translator (`dmsh_xlate`, its committed executable, which `shader build` runs), through SPIRV-Cross (linked in, at nxnative's commit) to Metal source, through the rewrites, entry points and reflection of `dmsh_build.py`'s modules, and through Xcode's Metal compiler:

```sh
$shader build -g 01007EF00011E000 --nxnative ../nxnative -o $w/shader/lib $w/shader/pkg
```

`--nxnative` names nxnative's checkout, where the profile folder (`dm/games/<id>`: `game.tsv` and the shader tables) and the translator (`dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate`) are; `--profile <dir>` and `--xlate <file>` give them on their own. The library is `<out>/b-<chain>.img` (chain `7c609af36b0f`, nxnative's `dm/gfx/nvn/dmsh_chain.h`, the version nxmac ports): every shader's `<key>_b.metal`, `<key>_b.json` (its reflection) and libraries (`_b`, and `_bx`, `_bf`, `_bv`, `_bp` where `dmsh_build.py` makes them), `_b_build.json` and the stamp `inputs.tsv`. Beside it (or in `--tables`) go `c1_full.tsv` and `cb_extent.tsv`, which nxnative's host reads from the profile folder. What it prints is `dmsh_build.py`'s: `[RED]` lines for a shader that does not build or a library left out, the summary. A shader that does not build is reported and left out; a stop (a package and a map from different splits, a translator of another chain, a library folder of another game) has exit code 1 and leaves no library.

The libraries are compiled for macOS 15 and later (`-mmacosx-version-min=15.0`, which the reflection's `chain_metal` records); `--min-macos none` compiles as nxnative does, for the SDK's own minimum, and the library is then the same files, byte for byte, as `dmsh_build.py`'s with the same tools (but for `_b_cas.txt`, the list of `dmsh_build.py`'s own cache entries). nxmac keeps its own cache in `<out>/cache` (`--cache`): every finished shader under the digest of everything that goes into it (its record and table rows, the tools, nxmac's own executable), and every Metal library under the digest of its source and compiler, so a second build of the same game takes each shader from there; remove the folder to take the space back. To compare with `dmsh_build.py` (nxnative's venv, as its docs/development.md builds the libraries) on BotW:

```sh
DM_GAME=01007EF00011E000 ../venv/bin/python3 ../nxnative/dm/gfx/tools/dmsh_build.py $w/shader/pkg $w/shader/lib_py
$shader build -g 01007EF00011E000 --nxnative ../nxnative --min-macos none -o $w/shader/lib $w/shader/pkg
python3 -c "import sys; sys.path.insert(0, '../nxnative/dm/tools'); import romfs
def files(p):
    with romfs.scandir(p) as it: return {e.name: romfs.open(e.path, 'rb').read() for e in it}
a, b = files('$w/shader/lib_py/b-7c609af36b0f.img'), files('$w/shader/lib/b-7c609af36b0f.img')
print(sorted(n for n in a.keys() | b.keys() if a.get(n) != b.get(n)))"   # only _b_cas.txt
```

## nxmac::shader

The library shader is built on (`src/shader`), a port of `shader_split.py` that keeps its order of work and its names: `scan.cpp` is its `scan_file` and `scan_blob`, `readers.cpp` its readers, `split.cpp` its `main`. Every file of the RomFS goes through the same dispatch, so that what is ported reads every file as `shader_split.py` does:

- the forms Breath of the Wild's files take: Yaz0, SARC (archives in archives, under the same budgets), BNSH files and the BNSH in effects, layouts and main, agl's BAHS packs and their program variations (`pso_pairs.tsv`);
- the other forms its generic scan takes in any file: gzip, Gust's zlib chunks, GLSLC compiler outputs, MSHD, NDSR and NDVE packs, sead's primitive drawer, NVN2 (Switch 2) control sections with task and mesh shaders, PlatinumGames' shader objects and DAT archives, Needle, Sumo, Star Fox's shader binaries, MonoGame effects in uncompressed XNB files, Phyre and RAGE's effects; and in main, YEBIS's zlib stream, control sections loose in the code and GLSLC outputs captured beside the RomFS;
- the keys (the profile's `shader_key_skip`), the pairs of variations and single-stage programs (the pixel input cover), the facts of compute shaders with the profile's hand-written ones, and the four tables and the package as `shader_split.py` writes them.

The RomFS is read through `nxmac::fs` (a DMROMFS1 image, or a folder) and main through `nxmac::rewriter`'s module loader. The checks of damaged data are `shader_split.py`'s: an offset or a length outside the bytes, read as Python's `struct` and slices read it, ends a file's scan with the same `[RED]` line and leaves that file out.

**Profile table.** `shader_split.py` takes `shader_groups`, `shader_note`, `shader_key_skip` and `modules` from the game's profile, and merges its `shader_facts_manual.tsv` over the facts it generates. `tools/export_shader_profiles.py` writes them for every nxnative profile to `src/shader/profiles.tsv`, which is compiled into the library; run it again when a profile's shader values change, then build (`shader -p <file>` takes another table, `shader profile <id>` shows what it holds).

**Not ported yet.** The readers of other engines' files: Unity (`unityfs.py`), Unreal (`uepak.py`, the shader code archives and Unreal Engine 3's caches), zstd files and HAL's size-prefixed frames (with Tears of the Kingdom's dictionaries), compressed XNB files, KTGL's archives and shader packages, RE Engine, MT Framework, Bezel Engine, Game Freak's GFPAK and Trinity packs, Retro Studios, Snowdrop, Criterion's Bundle 2, Heaps, Monolith, Next Level Games, Double Eleven, CRI's CPK, Camelot, PlatinumGames' PKZ, FromSoftware's dvdbnd, phx, RAGE's RPF6, Frostbite, zip archives, and the NROs a boot captures (`DM_NRO_DIR`). A file in one of these forms is left out with a `[RED]` line that says its reader is not ported (it counts among the files that could not be read); an archive `shader_split.py` takes apart as a whole before the scan (a zip, a CPK, an Unreal pak, a LINKDATA archive, Super Smash Bros. Ultimate's data.arc, ...) stops the split, and so do Tears of the Kingdom's zstd dictionaries and decoded Unreal shader code archives beside the RomFS.

**Tests.** `shader_test` holds the split to nxnative's: `tests/shader/fixtures/cases` has made-up games (no game data), one per form above, a DMROMFS1 image, several versions with DLC, damaged files and other engines' files, and `expected.tsv` and `lines.tsv` have the SHA-256 of every file `shader_split.py` writes for them and what it prints. `fixtures/src/make_fixtures.sh` makes them again (it needs nxnative and Python 3.12).

## nxmac::dmsh

The library `shader build` is built on (`src/dmsh`), a port of `dmsh_build.py` and the modules it runs a shader through, function by function and with their names: `rewrite.cpp` is `dmsh_rewrite.py`, `reflect.cpp` `dmsh_reflect.py`, `entry.cpp` `dmsh_entry.py`, `fetch.cpp` `dmsh_fetch.py`, `side.cpp` `dmsh_side.py` with `dmsh_alpha.py` and `dmsh_logic.py`, `misc.cpp` `dmsh_atomic.py`, `dmsh_wide.py`, `dmsh_vin.py`, `dmsh_pull.py` and the vertex capture of `dmsh_geometry.py`, `guard.cpp` `c1_guard.py` and `cb_extent.py`, `build.cpp` `dmsh_build.py` itself. The modules rewrite Metal source with some 270 regular expressions; `re.cpp` matches them with PCRE2 as Python's `re` does (its anchors, its stepping over empty matches, its replacement templates), and `json.cpp` writes the reflection as `json.dump(indent=1)` does. SPIRV-Cross's command line is compiled in (`cross.cpp`), so the same arguments give the same source.

What is ported is Breath of the Wild's path: vertex, pixel and compute shaders of NVN 1 (Maxwell), with everything their build gives them: the second translation of integer inputs, the MSL rewrites, the depth, capture, discard, transform feedback and layer entry points of a vertex shader and its padding for its pixel shaders, its shadow-input and pulling libraries; a pixel shader's integer, dual-source, alpha test, logic operation and readable-tile entry points and their libraries cut from one compile; a compute shader's first stores and shared memory past 32 KB; the reflection, `c1_full.tsv` and `cb_extent.tsv`.

**Not ported yet.** Geometry and tessellation shaders (the geometry stage as compute, `dmsh_geometry.py`; the mesh libraries of `geo_mesh.tsv`, `dmsh_mesh.py`), NVN 2 (Switch 2) shaders (`dmsh_ampere.py`, `dmsh_meshstage.py`, Ampere's HMMA), the iPhone's libraries (`IOSOUT`) and partial builds (`dmsh_build.py <pkg> <out> <key>...`). Such a shader is reported (`[RED]`, "not ported to nxmac yet") and left out.

**Translation chain.** The chain version names what nxnative's host loads (`b-<V>/`); nxmac's port is of the chain `7c609af36b0f`. `tools/export_dmsh_abi.py` writes `src/dmsh/abi.h` from nxnative's `dm/gfx/nvn/dmsh_abi.h` (the driver segment layout and the binding kinds) and `dmsh_chain.h`; when nxnative's chain moves, run it and port what changed in the chain's tools. The translator must be of the same chain: `shader build` stops when its embedded source digest is not the lock file's (`657232fd201f16a1`), and says so when the executable is not nxnative's committed build or the Metal compiler is not the lock file's.

**Tests.** `dmsh_test` holds the ports to the Python: `tests/dmsh/fixtures/calls.jsonl.gz` has 1,208 calls of the modules' functions, recorded while nxnative's own unit tests ran (`fixtures/src/record_calls.py`), with what each returned or raised, and every one is made again. `tests/dmsh/fixtures/build` is a made-up game, eleven Maxwell programs assembled by `src/gen.py`, whose library `dmsh_build.py` built (`expected/`); `BuildTest` builds it and must get the same files, byte for byte. Its translator is replayed (`bin/dmsh_xlate` gives each job what nxnative's translator gave it, recorded by `src/rec_xlate.py`), and its Metal tools are stand-ins (`bin/xcrun`, `metal`, `metallib`, python3 scripts that compile nothing but lay a library out as Metal's are), so the test runs on Linux too.

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
| Linux debug, asan, tsan | `ubuntu-24.04`, clang 18 | core, fs, the rewriter, the shader split and build, tools and tests (the host and gfx are Mac-only) |
| Mac debug, asan, tsan | `xcode-27` (Xcode 27.2, as nxnative) | everything, the host built for `GAME`; the tests, then an `nxhost` start |
| clang-format and clang-tidy | `xcode-27`, LLVM 23.1.4 (nxnative's lint-c.yml pin) | `format-check`, then the `tidy` preset |

The Mac jobs take the game's profile from nxnative at the commit `NXNATIVE_REF` names (a sparse checkout of `dm/games/<GAME>`), so nxnative's main moving does not change CI: raise it on purpose. `xcode-27` is GitHub's Xcode 27 preview image, which actionlint does not know yet (`.github/actionlint.yaml`). CI formats with clang-format 23.1.4: if Homebrew's llvm is another version and the two disagree, use CI's (`pipx install clang-format==23.1.4`, then `-DNXMAC_CLANG_FORMAT_EXE=$(which clang-format)`).

## Adding code

Where new code goes: the Switch's content formats into `src/fs`, the rewriter's rules into `src/rewriter` and a build's stub code into `src/rewriter/games/<title ID>.cpp`, the shader split's readers into `src/shader/readers.cpp` (and their dispatch into `scan.cpp`), the shader build's modules into `src/dmsh` (beside the Python function they port), the commands of a tool into `src/tools/<tool>/` (a library, so that its tests run it in-process, and a `main.cpp`); anything else without Apple APIs into `src/core`, with its tests in `tests/`; Metal and Objective-C++ into `src/gfx`; the host's own code into `src/host`. Every target calls `nxmac_target_defaults()` (`cmake/NxmacTargets.cmake`) for its warnings, ARC and clang-tidy. `src/core/title_id.*` is a first, small piece that proves the chain (Abseil, tests, sanitizers); it can stay or go.
