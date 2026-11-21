# Precompile all shaders (Windows and Linux, Vulkan)

"Precompile All Shaders" in a game's context menu is a per-game opt-in. The player waits once on the loading screen so that fewer shaders stall during play. It reduces shader stalls but can't prevent them: new graphics states, and compute dispatches whose reads differ from the guessed ones, still build during play (see below). It builds on the shader precache (#781, `src/video_core/shader_warm/`) and shader cache v2. It adds no new cache format, and nothing leaves the player's machine.

## What it does

1. Turning it on writes `<shader dir>/<title id>/warm-compute.requested` and sets the game's own `precache_shaders` to Always in its config (`custom/<title id>.ini`), which the game's properties change back. It then runs the precache build (`ShaderWarmDialog`, `BuildWarmFiles`) on every CPU thread. The build reads the player's own game file with their own keys. It extracts every NVN2 program from the RomFS shader containers (BFSHA, BNSH, SARC, CGSB, SHN, BAHS, effect sets, `.ptcl`, zstd with the game's dictionaries) and from asset packs (below). Then it lowers the compute programs and writes them to `warm-compute.bin`. On OoT this takes about 5 s.
2. On the next start of that game, the loading screen runs the precache before the game starts, whatever `precache_shaders` says. It also builds every graphics and compute pipeline the title's own cache v2 records hold, as `build_all_cached_shaders` does, but for this game only. Builds go through the normal `CreateComputePipeline` and `CreateGraphicsPipeline` on the shader job workers, so the driver's cache and Lichii's `vulkan_pipelines` file keep them. A matching `.done` marker (file contents, driver, device, cache version, SPIR-V emission stamp and settings) skips the compute precache on later starts until one of those changes.
3. Skip, or closing the loading screen, stops the precache. After a Skip, `warm-compute.progress` records the compute keys already built for that marker stamp, and the next start builds only the rest, but only when the builds persist: the driver keeps its own pipeline cache, or Lichii's Vulkan pipeline cache is saved. In the second case the progress is written only after that save succeeds, and it is read back only when the boot loaded the saved cache file. A boot that couldn't load it deletes the progress before it saves its empty cache. Otherwise the next start builds every compute key again. A closed load writes no progress, because it doesn't save the driver pipeline cache.
4. Turning the option off removes only the request; the game's `precache_shaders` stays as it is. "Delete Shader Precache" removes the request, the file, the marker, the count and the progress.

The global setting keeps its meaning: Off, Ask and Always apply to every game, and the request is the per-game override. MoltenVK and the other renderers are unchanged.

## Asset packs (Kirby Air Riders)

Kirby Air Riders keeps no shader archives. Its programs sit bare in `Asset/assets_a*.dat` (magic `PDAT`, 16.6 GB): there are 194,537 zstd members, and each is a FlatBuffer holding one compute program or a vertex and pixel pair. The reader is a port of Zachary McKenney's #1117 (40dadda7d5, b57bfc0457), without its recipe stack:

- `IsFramePack`: a RomFS file the archive readers don't take is a pack when it starts with `PDAT`, or when it is 1 GiB or larger and isn't media.
- `ScanFrames` reads the pack through bounded VFS reads, 64 MiB at a time, never the whole file. A zstd frame must parse to its end (`ZSTD_findFrameCompressedSize`) before it is inflated, so a stray magic costs nothing. The stop token is checked before each window. A read that returns less than asked before the pack's end fails the build like a stop, and the previous precache file stays.
- `FindBarePrograms` reads each inflated member. A member is a native control section (magic `0x98761234`, version `0xc9`), its own length at +0x30 and its code length at +0x44, followed by the code. The stage comes from the control's stage byte (+0x56: 5 compute, 6 task, 7 mesh) or from the graphics header at the code's start.

Three things in the original scanner are fixed, each with a test:

- A magic cut by a window's end was lost. Each window now starts 3 bytes before the last one ended, or after the last frame taken.
- A frame larger than the window was skipped. It is now read on its own up to 256 MiB (`max_frame`, which also bounds frames that fit in a window). A frame that declares its content size is first read to `ZSTD_compressBound` of it, and again up to 256 MiB when it is larger (a streaming writer's many small blocks).
- A file was read past its first GiB only when that GiB held a frame. A pack is now read to its end.

Frames that parse but don't inflate, and frames too large to read, are counted and skipped. Both the game-list action and the launch-time rebuild call `BuildWarmFiles`, so they share one reader. `WarmBuilderVersion` 4 makes the precache file of every precached game build again once, so Kirby's 12-byte file gets its programs.

## What can be precompiled from game files alone

| Work | Depends on | From game files? |
|---|---|---|
| Container walk, decompression, program extraction | Game files | Yes |
| Compute program lowering and Maxwell to IR to SPIR-V | Program, control section (workgroup, shared and local memory), constant buffer words, texture types and storage pointers read at dispatch | Yes, with the reads it can't know guessed (`GuessMissing`: texture bank 0, the typed defaults). It hits at runtime only where the guesses match, so the driver sees the same SPIR-V. |
| Compute driver compile (`vkCreateComputePipelines`) | The SPIR-V above and the device | Yes, under the same condition |
| Graphics stage translation | Previous stage outputs, vertex attribute types, topology, transform feedback, depth mode, render target formats (`RuntimeInfo`, the graphics key) | No. Without the draw state the SPIR-V isn't the one a draw asks for. |
| Graphics pipeline compile | The whole graphics key: blend, depth, rasterizer, vertex input, dynamic state flags | No, except for states the player's own records hold. The option never invents a state. |

The Direct NVN path (`nvn_direct`, `nvn_recompile.c`) isn't on nx2: it lives on `nvn-direct/lichii`. Its translation cache keys a program by its key plus primitive inputs, stage, depth mode and vertex input types, and translation reads constant buffer words and texture types at the draw. So the same split applies: compute programs could be seeded from the game files with guessed reads, and graphics stages can't. That is follow-up work on that branch.

## Expected benefit

Measured on the EGLA-304 cold OoT capture (nx2 6d75240938, RTX 5070 Ti, `precache_shaders` Off, async shaders and Bounded on). Frames over 33 ms had 153.7 s of time over budget:

- Pipeline waits: 56.5 s. Of that, 15 compute dispatch waits make up 51 s, and they are 13 of the 14 frames over 1 s.
- Graphics misses: 2.3 s, with no frame over 1 s.
- Not shader related: the remaining 95 s (texture, buffer, query and other).

So shaders made 38% of the over-budget time and nearly every freeze, and the compute share is the part this option can remove. Kirby Air Riders is the same pattern, only worse. Build 338 on an RTX 3080 with no cached records had 46 compute dispatch waits over 1 s, up to 7.7 s and 179 s in all (EGLA-349). Until now the precache found none of Kirby's programs. #1117's reader found 8,091 compute programs on retail 1.3.3, among them all 745 that one recorded race used. EGLA-75 measured that the game-file precache covers 95% of OoT's and 70% of MKW's recorded compute keys. Whether the guessed reads give the same SPIR-V as play has not been measured (#579 claims 95%). Since #2104, an unbuilt compute dispatch can be skipped instead of waited for, so a precached program also avoids a skipped effect. Kirby Air Riders' compute programs now come from its asset packs. Games whose programs neither the container readers nor the asset pack reader find get only the recorded-pipeline half. Before the asset pack reader that included Mario Tennis Fever, which hasn't been rechecked with it.

## Measuring warm hits on a real dump

A program found isn't a hit. It counts only when the warm file's key (code hash, shared memory, workgroup) equals a key the game dispatches. Run this on the owner's machine with a scratch profile:

1. Play one race on the build under test with a cold cache. The title's `lichii-cache.v2.bin` then holds that race's compute pipelines.
2. Turn on Precompile All Shaders for the game. The log line `Shader precache: Asset/assets_a000.dat: N zstd frames, F failed, T too large, P new compute programs` gives what the pack held. `eden-cli --build-warm <game file> 040077201BE50000 <dir>` gives the same counts without the frontend.
3. Start the game. The `Shader precache: J of N compute programs to build ... (H of the title's C cached compute pipelines in the file ...)` line gives the exact hits: H of the C race pipelines have a matching entry in the file.
4. To see whether the guessed reads give the same SPIR-V, isolate the driver cache first. Steps 1 and 3 filled it with the race's exact pipelines, so builds from it say nothing about the game-file precache. Clear the driver cache (on NVIDIA the folder in `__GL_SHADER_DISK_CACHE_PATH`, elsewhere the title's `vulkan_pipelines` file), the title's `lichii-cache.v2.bin`, and `warm-compute.done` and `warm-compute.progress`. Keep `warm-compute.bin` and the request. Start the game and let the precache finish: it fills the driver cache from the game files alone. Keep that driver cache, then play the same race with `EDEN_NX2_STUTTER_TRACE=1` and `EDEN_NX2_PIPELINE_BUILD_TRACE=<file>`. Compute `END ... ms=` values under 10 ms suggest driver-cache hits, but build time is only a proxy, not proof of a hit. `Compute dispatch waited` lines over 1 s are misses. Compare them with build 338's 46.

## Licences

The asset pack reader is ported from Zachary McKenney's GPL-3.0 Lichii #1117, with credit in the source. The extractor is Lichii's own C++ port of PR #214's Python extractor. Its BNSH layout follows the MIT-licensed ShaderLibrary, as `shader_archives.cpp` credits. It contains no nxnative code. For reference: nxnative at 7ebb76eda8ec is MIT (seedds). Its bundled `spirv_cross_bin` is SPIRV-Cross under Apache-2.0, with MIT, CC-BY-4.0 and Khronos free-use notices. All of these may be redistributed in a GPL-3.0 app with their notices, but nothing here bundles or ports them.

## Rules

Everything stays in the player's shader folder. Nothing downloads, uploads or ships pipeline data, recipes or caches (#467, and the 2026-11-09 rule against shared recipe files). The option only ever reads the player's own game file and the player's own cache records.
