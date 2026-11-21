# Kirby Air Riders white-world investigation

Program ID: `040077201BE50000`.

The changes fix three reproduced shader rejection mechanisms. They do not yet establish
which failure causes the white world, so an in-game run is still required. No
texture-format mapping is added.

## Evidence and fixes

The original `0bb0445671` log rejects a geometry program at instruction `0x9130`:
`08007950 ffff6ec0 03c3ffff 000fea00`. The engine's stage slot 4 is geometry,
not fragment. This is an unconditional `RET.REL.NODEC R8` whose relative base is zero.
The instruction cannot fall through into the exit handler. The same failure appears in
two programs in the subsequent `cb5b085c34` probe, at offsets `0x9130` and `0x9180`.
The log immediately records skipped draws into a 2048x1152 render target.

The geometry envelope validator previously required an unconditional EXIT immediately
before its handler, rejecting a valid return from a preceding subroutine. Allow the
verified return encoding, retain the lowerer's return-link checks, recognize the end
of the main body, and keep lowering through the handler after the subroutine returns.
Call-link tracking is enabled only for envelopes containing returns, so existing
return-free envelopes keep their previous behavior. Predicate, return-base, linkage,
handler-body, and branch-boundary checks remain in place.

The original log also rejects compute instruction
`5fdc7220 00000052 00410000 000fe200` at `0x36f0`. This is FMUL with destination
R220 and inputs R95/R82. R220 overlaps the lowerer's uniform-register mirrors, but
the guest-register preservation table did not include opcode `0x220`. Add its three
register operands to that table. A regression checks both operand remapping and the
translated result of multiplying 2 by 3 through the shadow register.

## Atomics behind unmapped pointers with a carried offset

In the `kirby-white3` run with render tracing, every traced draw (1,078 draws) is
configured. The only rejected GPU work is compute shader `0x1175573eed7481a4` (pipeline
`0xced45eff3d2d0513`, program `0x11c7811e00`). It fails with `Global atomic add with no
routable storage buffer`, and both of its dispatches at about 31 seconds are dropped.

The program has 70 `ATOMG.E.ADD` instructions. In the IR, 65 of them add an immediate
`0x1c` (and `+8`) to the 64-bit pointer at `c0[0x40]` in 32-bit halves. The carry
is computed as `low < pointer_low` and is added to the high word. The other 5 add a
thread-dependent 64-bit offset to the pointer at `c1[0x0]`; their carry is the add's
carry flag. Both pointers are classified unmapped when the pipeline is built.
[Lichii #47](https://github.com/zortos293/Lichii/pull/47) keeps the buffer only for a
pointer that is the atomic's bare address, so neither form matches. Offline, mapping
only `c0[0x40]` routes the 65; mapping only `c1[0x0]` routes the 5.

`CollectAtomicPointers` now also accepts a pointer when the high half of the atomic's
address sums the pointer's high word with a carry select whose condition reads the
pointer's low word. A 32-bit scalar added to a pointer carries nothing, so #47's case
of an atomic offset by `c0[0x40]` stays global. Upstream `nx2` still has the
throwing stub and no open or closed PR changes this routing.

The original log has 58 format-zero assertions, but the probe's white menus occur
without that assertion. Format zero is the hardware descriptor's INVALID encoding,
and component type 2 means UNORM; these fields do not identify an omitted valid
format. See NVIDIA's `NVC697_TEXHEAD_BL_COMPONENTS_SIZES_INVALID` in
[clc697tex.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clc697tex.h).
Full descriptor, handle, pool index, dimensions, address, and mapping traces have been
added rather than assigning a guessed format.

Upstream nx2, PR/issue searches, and relevant texture/shader diffs were checked.
Upstream does not contain the fork's `geometry_exit.h` or `register_pressure.h` helpers.
The X8Z24 fix from Eglantine #295 and the texture-header-offset fix are already present;
neither describes this format-zero entry. The subsequent upstream sync was merged
through origin/nx2, including #47, #48, #49 and #50.

## Ruled out: W scale factors and the graphics scale

The `f9dff5c28c` run builds every shader and still shows the white world. 83 of its 501 dumps
read `SR_WSCALEFACTOR_XY` and `SR_WSCALEFACTOR_Z` (S2UR 30 and 31): 82 graphics programs and
compute `0x130d607b00`. In the vertex programs the factors scale W for frustum tests that
choose an early EXIT, and lowering removes that rejection, so no IR reads the factors. None of
the 50 cached modules that contain the stub's `0x00ff0000` constant bitcasts it to a float;
each uses it only as a bit mask. The open upstream Eglantine #27 maps the factors to the
viewport clip-control guardband (256.0 or 1.0), but that would change no emitted module here.

`nx2:IGraphicsScaleController` command 1050 is `nn::oe::SetGraphicsScaleChangedNotificationEnabled(bool)`
(seedds/nxnative `dm/host/hle_oe.c`). It now has a handler instead of the auto-stub. Its reply
was already an empty success. Command 10 still returns 10, the handheld tier, which Donkey Kong
Bananza uses only as a quality tier.

## Verification

- Clang/Ninja shader build: all 199 Ampere and `[storage_buffer]` test cases pass
  (8,102,012 assertions). The new carry test fails without the routing change.
- The failing compute dump, compiled offline with no pointer mapped, emits 70
  `OpAtomicIAdd` on two storage buffers (`c0[0x40]`, `c1[0x0]`), with no global atomic
  left. The module passes `spirv-val --target-env vulkan1.2 --scalar-block-layout`.
- A sweep of the `kirby-white3` dumps plus the v48 cache (1,231 jobs) emits SPIR-V
  for all 1,038 modules, byte-identical to the output before the routing change.
- All three standalone shader-sweep suites passed, including a synthetic geometry
  CALL/RET/exit-handler program that emits a vertex and passes `spirv-val`.
- The Kirby v48 cache has 1,038 validated compiler cases and 94 cases missing
  captured context. Cached programs are already lowered; they cannot recover
  native programs rejected before caching.
- The four runtime translation units affected by diagnostics compile with clang.
  The generated GNU-only `-mtls-dialect=gnu2` flag was omitted for clang 18.
- All 48 additional geometry captures still fail at their original setup-liveness
  checks, not the RET/handler boundary: Raiders has 6 BRA, 2 predicate-logic and 2
  predicated EXIT cases; Duskbloods has 11 ALD, 16 texture, 9 uniform-add and 2
  floating-compare cases. No unrelated setup-liveness change is included.

## Next in-game capture

Set `EDEN_NX2_SHADER_DUMP` to an existing writable directory and
`EDEN_NX2_RENDER_TRACE=1`. Use log filter
`*:Info HW.GPU:Debug Render.Vulkan:Debug Shader:Debug`.

Besides native shader binaries, the dump directory now receives
`failed-compute-1175573eed7481a4-v49.env` if that compute pipeline fails and its
environment can be serialized. Replay it with:

```sh
build/shader-sweep/shader-sweep \
  --environment failed-compute-1175573eed7481a4-v49.env --cache-version 49 \
  --output shader.spv
```

Review the log and screenshots from both a menu and a race before sharing them. The dump directory
contains game shaders: do not publish it or attach it to an issue. Ask a maintainer for handling
instructions, and share only material you have permission to disclose. Check whether the compute
rejection is gone and whether the 3D world returns with it.

## Root cause: the SMAA resolve's dither bias

The first frame dump (frame 1338, character select) shows the 3D scene rendering correctly up to
pass032. Pass037 (VS `eb21e6be1d399dda`, FS `f2eb2333f228f72c`, A2B10G10R10 864x480) is the
first white image: 98.84% of its pixels are exactly (1,1,1,1). The remaining 1.16% are a strip
that the pass never writes.

That FS is an SMAA neighborhood-blend pass. It reads two textures (the blend weights and the
color) and the texel size from bank 17 (root table 0, which the blit-constant pass fills).
It encodes the result to sRGB. It then adds bank 17 words 0x10, 0x14 and 0x18 to the output
*bits* as integers. On the console these words are the dither bias and hold zero. The blit
block holds the texel size and a UV scale of 1.0 there instead. Integer-adding 1/864, 1/480 and
1.0 to float bits overflows the exponent, so every written pixel saturates to white. The
dispatch and upscale that follow keep it white.

`BlitConstantsPass` already dropped this bias, but only when the color operand was a direct
`BitCastU32F32`. Here the operand is a phi of the two sRGB branches. The pass now also accepts a
phi whose incoming values are all float bit patterns. An integer, or an immediate added to the
bias, is still kept.

In the Kirby sweep, two of 1,038 SPIR-V modules change: this FS and its sibling `f7630c142a3b7203`,
which uses the same VS. All results and statuses are otherwise identical.

## Black characters: incomplete texture handles

The owner confirmed that the white-world fix works on `26a608918e`, but characters still
render dark. The earlier `kirby-white4` trace records fragment shader
`16ad27bb614d3e6e-gfx-130daf4e00.bin` querying handle `0x24000000`. This selects texture
index zero and sampler `0x240`. Its TIC is a null entry, not an unrecognized compressed format:
`0000000000024900/0000000000600000/0000000000000000/0000000000000000`, image address zero,
dimensions 1x1x1, format INVALID, UNORM components.

The captured shader contains a depth-comparison sample whose handle is
`c6[LoadStorage32(...) << 4] | c2[0x570]`. Static handle tracking cannot represent the first
operand. It then searches inside the OR and incorrectly treats the sampler operand alone as
the entire handle. That index can depend on fragment inputs, so evaluating one handle on the
CPU for the whole draw is not correct either. The fix uses the existing shader-indexed descriptor
arrays for zero-based cbuf tables, composes a dynamic texture table with a constant sampler,
and keeps that sampler's cbuf offset fixed when binding each array element. Tests check both
operand orders, the retained shader-side index, and the unchanged fully static path. An owner
run is still needed to establish how much of the character/shadow problem this explains.

Vulkan recreates SPIR-V from cached environments, then supplies the new modules and descriptor
layout to `vkCreateGraphicsPipelines`; the application's pipeline hash does not select an old
driver binary directly. However, old environments omit the newly needed cbuf/texture reads.
After a cache-load failure, the old disk-key set also prevents a live recompile from replacing
that record. Vulkan cache version 50 and OpenGL version 46 therefore start fresh versioned files,
without deleting the previous files. Expect a cold-cache first run.

The initial array cap runs before final uniform and static texture counts are known. A final
budget check now includes those resources and any NVN storage buffers, then tightens shader-side
index clamps when reducing array counts. A metadata-only audit of the nine incomplete cached
environments found four over-budget cases at a synthetic 1,024-resource limit. Their final totals
are now 1,023 or 1,024. Audits at 128 and 1,048,576 also fit all nine; at 32, one cannot fit even
one element per array and is rejected. Missing context was substituted only for this structural
audit. The nine shaders remain unverified with real runtime data.

The `kirby-fix4` log does not establish the cause of the low frame rate. Its 636.1 ms pipeline
summary spans 10.070 seconds and counts GPU-thread translation, not driver-worker compilation.
Between 118 and 125 seconds the individual logged builds total 2,121.1 ms. The last logged
build is at 124.556 seconds; the log continues to 157.936 seconds. Profile another run with
`EDEN_NX2_GPU_PROFILE=1 EDEN_DKB_FRAME_TRACE=1`; exclude the deliberate frame-dump stall from
performance measurements.

### Sustained GPU stalls

The next capture contains multi-second GPU work after the frame dump. Renderer frame 5221
records 3,809.697 ms of submission execution across a 3,835.030 ms span. Frame 5231 records
5,823.947 ms across 7,608.752 ms. These timestamps establish expensive submitted GPU work,
but do not identify the shader responsible. The captured present image is a sky transition,
so it cannot verify the remaining black characters or broken shadows.

For another performance capture, keep the shader cache and enable
`EDEN_NX2_GPU_PROFILE=1 EDEN_DKB_FRAME_TRACE=1`. Do not enable a frame dump in the measured
interval. The profile adds `writer wait`, `list submit wait` and `list GPU wait` zones to
separate front command-list readback waits from command parsing. Existing GPU timestamp
samples also report up to four submissions taking at least 10 ms as `DKB gpu_submission`.
Each line includes the submission tick, draw and dispatch attempts, and up to 32 distinct
stage/hash pairs. Stages 0 through 5 match the graphics pipeline key; stage 6 is compute.
`untracked_uses` counts shader uses omitted after the list fills. Attempts can include work
skipped because its pipeline was not bound. Transfer and emulator-internal work are not
attributed to guest shader hashes. The duration belongs to the whole submission, not to
each listed shader.

The same sampled submissions also time each direct and indirect draw and dispatch with a
pair of timestamps. `DKB gpu_work` lists the eight shader combinations with the most GPU
time: `s6:<hash>` for compute, `s1:<vertex>+s5:<fragment>` for draws, then the total, the
number of calls, the slowest call and its grid or vertex and instance counts (zero when
indirect). The timestamps wait for earlier work, so draws in a sampled frame do not overlap
and their sum exceeds the submission time. Use it to rank work, not to measure the frame.

## Frame dump

The logs show every shader building, so the next step needs pixels. `EDEN_NX2_FRAME_DUMP` reads
back one whole frame inside the emulator. No capture tool is needed.

| Variable | Value |
|---|---|
| `EDEN_NX2_FRAME_DUMP` | Directory for the dump. It is created if missing. When unset, the dump code stays inactive. |
| `EDEN_NX2_FRAME_DUMP_AT` | Which frame to capture: seconds after boot (`45`), a frame number with an `f` suffix (`2700f`), or `file` to capture once `capture.now` exists in the dump directory (checked every 30 frames). Default `60`. |

Example for the character select screen:

```
EDEN_NX2_FRAME_DUMP=C:\lx\dump EDEN_NX2_FRAME_DUMP_AT=45
```

Open character select, then wait past the trigger time. The log shows `Frame dump: capturing frame
...` and then `Frame dump done: N images ...`. The frame stalls for a few seconds while the files
are written.

For that one frame the dump writes:

- the color targets of every pass when the pass ends. A pass is a run of draws and clears to the
  same framebuffer, and it also ends before each compute dispatch and before presentation;
- every storage image that each compute dispatch writes, read after the dispatch;
- the image the frame presents (`AccelerateDisplay`).

Each image produces `<name>.raw.zst`, which holds the exact texels with tightly packed rows. For
the common color formats it also produces `<name>.png`. Each image gets one line in
`manifest.jsonl` with these fields:

- pass or dispatch index;
- the draw range;
- the shader hashes (the same hashes the shader dumps use);
- format, size and mip/layer;
- render-target GPU address;
- whether the image is rescaled;
- per-channel min, max and mean, and NaN and Inf counts.

The file names carry the same tags. The PNG previews encode float targets to sRGB and clamp them
to [0,1], and they show NaN or Inf pixels as magenta. MSAA targets are listed but not read back.
Writing stops at about 500 MiB.

Build the contact sheet with:

```
python3 -I tools/nx2/frame_dump_index.py <dump dir>
```

This writes `<dump dir>/index.html`: one card per image, in capture order. Badges mark constant
images, values above 1 and NaN/Inf. The first pass whose output turns white or constant is the
pass to look at.
