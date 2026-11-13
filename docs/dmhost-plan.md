# The dmhost rewrite plan

nxhost (`src/host` + `src/gfx`) is the C++ rewrite of nxnative's Mac host, `dm/host` + `dm/gfx` (dmhost). This plan was measured at nxnative f20f6507 on 2026-11-13.

## Size

- About 150 source files, 4.5 MB of hand-written C/ObjC (about 54k long lines). They hold 1,078 HLE takeovers (`HLE_FORCE` / `HLE_FORCE_BIG`). A further 0.4 MB is generated (`dm/gfx/tools/gen_protos.py`: `nvn_thunks.S` and `nvn_protos.h`, 743 names). `nvn/include/nvn.h` (86 KB, MIT) is kept as it is.
- The tests are 212 files, 2.4 MB: `dm/host/tests` has 69 files (694 KB) and `dm/gfx/tests` has 143 host-side ones (1.68 MB, Metal). The 53 Python tool tests in `dm/gfx/tests` are not part of this plan.
- For scale, nxmac's `src/` was 1.0 MB of C++ when this plan was written. `nvn_mtl.m` alone is 1.57 MB.

## Decisions

- **Scope: BotW first, then everything.** Items 1–11 make Breath of the Wild (`01007EF00011E000`) run end to end on the Mac. Items 12–13 come after. Until they do, the parts they cover report "not ported yet".
- **Approach: nxmac's own code.** nxhost is written in nxmac; nxnative's `dm/host` and `dm/gfx` are the reference that is read, and none of nxnative's own code (its sources, tools or shaders) is compiled into nxhost. What does come from nxnative is what nxmac only refers to: the game profiles (`dm/games`) and the third-party code nxnative vendors (libvpx, dav1d, and later stb_vorbis, Vulkan-Headers, `nvn.h`).
- **Small steps.** Every change is small and checked before the next one: unit tests (in the cloud where the code has no Apple APIs), then a build on the Mac, and a check on BotW's real files where the step can have one (the loader on BotW's rewritten modules, for instance). BotW itself can only start once items 2–11 are in place; then the BotW smoke run (the profile's `smoke.tsv`) joins the checks. Off the Mac, code with Apple APIs can only be compile-checked (`python -m ziglang c++ -target aarch64-macos`, without the SDK's frameworks).
- **Minimum macOS: 15, nxhost included** (as the rest of nxmac). nxnative's graphics code uses Metal 4 (macOS 26) throughout, so its port is guarded for availability, and nxhost runs only on macOS 26 until the graphics code has a path without Metal 4. That path is not one of the items below yet.
- **Xcode 26:** nxmac builds with Xcode 26's SDK. What nxnative takes from Xcode 27's SDK (MetalFX's content offsets in `nvn_mtl.m`, for one) goes behind `#if defined(__MAC_27_0)` as well as `@available` in the port.
- **Port style (as nso and the shader build):** a faithful port file by file, keeping names, behaviour, log lines and environment switches. Inside, it uses C++ idioms (RAII, std, Abseil), with Objective-C++ only where Apple APIs are touched. Each item's tests move to GoogleTest with it.

## Items

Sizes are of nxnative's source.

| # | Item | nxnative files | Size | BotW |
|---|---|---|---|---|
| 1 | Build plumbing: libvpx 1.17.0 (VP8 / VP9 decoders) and dav1d 1.5.4 (+ NEON assembly) from nxnative's vendored copies. The rest comes with the item that needs it: stb_vorbis 1.22 (9), `nvn.h` and nxmac's own generator of the NVN prototypes and thunks (10), the host's Metal shaders compiled by nxmac::gfx (11d), Vulkan-Headers 1.4.363 (13) | Makefile | small | yes |
| 2 | Core runtime: `main.c`, `loader.c`, `entry.S`, `hle.c`, `hle_table.c`, `hle_bigstack.c`, `crash.c`, `probe.c`, `thread.c`, `hle_thread.c`, `hle_svc.c`, `hle_os.c`, `hle_sdkinit.c`, `hle_ro.c`, `hle_jit.c`, `hle_guestthread.c`, `hle_count.c`, `hle_fdstat.c`, `memstat.m`, `dm.h`, `hle.h` | 21 | 454 KB | yes |
| 3 | File system and saves: `hle_fs.c`, `save_sync.c/.h`, `romfs_image.h` (on nxmac::fs's DMROMFS1), `romfs_identity.h` | 5 | 146 KB | yes |
| 4 | System services: `hle_oe`, `lifecycle`, `time`, `account`, `friends`, `mii`, `offline`, `misc`, `net`, `web` + `web_apple.m`, `sf`, `sdk16`, `swkbd`, `nvdrv`, `glslc` | 16 | 324 KB | yes |
| 5 | Game hooks: `hle_fpslock` (BotW), `hle_uecvar`, `hle_uemalloc`, `hle_oodle` | 4 | 67 KB | fps lock |
| 6 | Input: `hle_hid`, `touch`, `haptics` + `haptics_apple.m`, `pad_*` (GameController, HID Pro Controller, Switch 2 over Bluetooth, `switch2_proto.h`), `motion_*`, `dm_input.h`, `ctrl_prompt.h` | 20 | 306 KB | yes |
| 7 | Mac front end: `front.h`, `front_mac.m`, `home_quit.h`, `home_quit_mac.h` | 4 | 147 KB | yes |
| 8 | Audio: `hle_audio`, `hle_audren` | 2 | 115 KB | yes |
| 9 | Movies and codecs: `hle_movie` (WebM: VP9 + Vorbis), `movie_avc` (VideoToolbox), `movie_av1` (dav1d), `codec`, `codec_video` | 8 | 187 KB | VP9 + Vorbis |
| 10 | NVN interface: `hle_nvn.c`, `nvn_obj.h`, `nvn_rec.h`, `nvn_dirty`, `nvn_tex`, the queue / hmap / count / timestamp / pso_skey / pso_ds headers; `hle_vi.c`, `hle_nv.c` | 15 | 485 KB | yes |
| 11 | NVN replay on Metal: `nvn_mtl.m` and its helpers, `nvn_refl`, `host_msl`, `msl/*.metal`, `tex/` (deswizzle), `metal/mtl_backend` | 41 | 1.75 MB | yes |
| 11a | Memory pools, submission, sync, events, counters, queries, conditional render | | | |
| 11b | Textures to `MTLTexture`, samplers, descriptor pool, deswizzle, sparse textures | | | |
| 11c | Render passes, draws, clears, compute, copies, indirect draws, 8-bit indices, indirect-count multi-draws | | | |
| 11d | Shader library: `b-<V>.img` (nxmac's `shader build` output), mapping, reflection, `c1_full` / `cb_extent` tables, host MSL | | | |
| 11e | Pipelines: PSO cache, Metal 4 bases and archive, parallel precompile, hybrid pipelines, prebuild, prepare step | | | |
| 11f | Present, present thread, pacer (`nvn_pace.h`), MetalFX upscaling | | | |
| 11g | Pass optimizations: store lookahead, tile merges, same-pass feedback, attachment ledger, zero-source skip, CS hoist | | | |
| 12 | NVN extras: `nvn_geometry.inc` (geometry emulation), NDVE (`nvn_ndve.inc`, `ndve_engine.metal`), capture and replay, diagnostics (TEXDUMP, PASSGPU, A/B, bootprof) | 7 | 200 KB | later |
| 13 | `hle_cheats.c`; the Vulkan layer `vk/hle_vk.m` (Hades); the OpenGL layer `gl/hle_gl.m` (Digimon) | 3 | 426 KB | no |

- BotW end to end is items 1–11: 17 pieces of work (items 1–10, plus 11a–g), about 3.9 MB of source.
- Order: 1, 2, 3, 4, 5, 7, 6, 8, 9, 10, then 11a–g, then 12 and 13.
- Out of scope: the Mac app launcher (`dm/mac`: `launcher.c`, `Launcher.swift`, `pack_app.py`; 55 KB) and iOS.

## Things to keep in mind

- ABI: Apple's AAPCS64 and the Switch's differ. Arguments narrower than 32 bits are received as `int` / `unsigned` and truncated by the host, and variadics are never C's `...`. No C++ exception may cross a guest frame.
- Registration: the `HLE` / `HLE_FORCE` tables live in the Mach-O sections `__DATA,swhle` / `__DATA,swforce`. If no module references an entry, loading fails, on purpose.
- A piece is done when it builds on the Mac, its tests pass there (as GoogleTest), its check on BotW's files (if it has one) passes, and, once BotW can start, the BotW smoke run behaves as dmhost's.

## Status

| # | State |
|---|---|
| 1 | done: libvpx as `nxmac::vpx`, dav1d as `nxmac::dav1d` (`src/host/third_party`); the rest comes with items 9–13 |
| 2 | in progress. Done: 2a, the layout tables (`<module>.fixups.tsv`) read by `nxmac::loader` (`src/loader`, portable, `loader_test`); 2b, `nxhost <dir> [mod...] [-- game arguments...]` picks the profile's modules (main.c `profile_modules`), reads their tables and stops there, logging `[dm]` lines as nxnative's host. Next: 2c, mapping the images (`<module>.patched.bin`) |
| 3–13 | not started: nxhost is still the stub `src/host/main.cpp`, `src/gfx` has only `metal_device.mm` |
