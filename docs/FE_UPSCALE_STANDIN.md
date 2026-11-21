# Fire Emblem: Fortune's Weave spatial upscale candidate

This is a narrowly scoped workaround for the repeated tree/tile trails reported in
Fortune's Weave, not a proven repair of software DLSS emulation.

The upstream substitute upscaler was only reached after a matrix-shader rejection.
Restoring HMMA support removed that rejection and made the game run its native DLSS
network. The preserved trailing-session logs never took the substitute path. That
establishes a routing difference, not which network instruction or input is wrong.

## Routing

- Only title `04007EF01F8AE000` and captured native output-program fingerprint
  `14b4222bbe23abee` qualify. Other programs, including intermediate network layers,
  retain normal execution.
- The fingerprint is retained with the in-memory lowered program. Cold and warm
  shader lookups check it before a pipeline-cache lookup. No cache deletion or
  shader-cache version change is needed.
- The substitute uses cbuf0 handles at `0x328` and `0x348`, only with a mapped,
  enabled constant buffer and mapped descriptors. Both descriptors must be distinct,
  nonzero, block-linear, single-layer, non-mipmapped 2D B10G11R11 images, with the
  output larger in both dimensions.
- A successful spatial upscale replaces the final dispatch. The identified prepass
  (`4cbf6eff32525aec`) supplies this frame's input-colour identity and camera jitter.
  When these match the output pass's input, a fractional-pixel correction removes
  jitter before bilinear sampling. No motion vectors or previous-frame history are
  sampled by this path. The current image is stored for same-frame RCAS sharpening.
  Completion is handled separately from shader
  rejection and is never added to the rejected-program cache.
- If the pair does not qualify or the renderer cannot accelerate the copy, the
  original shader dispatch remains enabled.
- Existing fallback behavior for other games is unchanged.

## Same-scene verification

The temporal path described in [Game-DLSS replacement](native-dlss-replacement.md) is the
default. This spatial workaround runs only for frames without matching motion vectors and
depth, or with `EDEN_NX2_DLSS_REPLACEMENT=spatial`. On its own, it made the image soft and
its edges crawl from frame to frame (all-in-one-5 report, RX 6650 XT).
`EDEN_NX2_DLSS_REPLACEMENT=native` restores the native path for a fresh process.
The setting is process-local and does not modify saved configuration.

Look for `Fire Emblem DLSS replacement: applied` in the log with no replacement override.
`unavailable; retaining native dispatch` means the workaround did not
apply; do not treat that run as evidence for or against the DLSS hypothesis.

Use the same save, camera motion and scene for both runs. If the trails vanish,
investigate the native network and its inputs further. If they remain despite the
confirmation log, capture the input colour and depth before this pass.

The original spatial-only candidate removed the reported smearing but exposed camera
jitter (user report). The de-jitter candidate consumes each prepass sample once and
rejects non-finite/out-of-range values or mismatched input images. Missing samples
retain the native dispatch rather than applying an uncorrected copy.

This spatial substitute may look softer or more aliased than correct DLSS.
It deliberately avoids adding a second unverified temporal-history path.
No FPS gain or gameplay-correctness claim is made.

## Checks

`[fe_upscale]` tests title/fingerprint gating, opt-out, resource validation and the
separation between substitute completion and shader rejection. These do not test
a real Vulkan copy or establish that the screenshot is fixed.

The original spatial-only Windows Clang 22.1.8 Release candidate built successfully. The focused
`[fe_upscale],[command_fetch_order],[native_reset],[multi_range_refresh]` run passed
16 cases / 1,267 assertions. The full suite passed 220 of 221 cases; the existing
NUP writer hash assertion at `src/tests/core/nup_writer.cpp:391` still failed.
All five preserved copies of the identified native output program matched the
fingerprint used by this workaround. The user subsequently reported that smearing
was removed but the screen shook. The de-jitter revision still requires a same-scene
runtime check; its tests cover sample consumption and invalid-value rejection.

The de-jitter Windows Release build also succeeded. Focused checks passed
18 cases / 1,288 assertions; the full suite passed 222/223 cases with the same
NUP hash failure. The modified host shader compiled and passed `spirv-val` for
Vulkan 1.2. All sampler reads use explicit LOD zero, matching the blit sampler.
No same-scene gameplay check of this revision has been performed.
