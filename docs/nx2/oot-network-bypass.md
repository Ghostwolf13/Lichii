# Ocarina of Time: bypassing the game's anti-aliasing network

The Legend of Zelda: Ocarina of Time (`0400486024DEC000`) runs a tensor network on its tonemapped
image followed by an output pass that sharpens hard. The No sharpening option
(`oot_anti_aliasing`) shows the tonemapped image and can also skip the update's network passes.

## What the game does

The update (1.1.1) was inspected in Kokiri Forest, docked at 1x with the built-in Max Resolution
patch. Sampled frame dumps, constant-buffer traces and pixel statistics showed:

- The 3D renders at the output size (2560x1440 docked), with no jitter and no motion vectors.
- The scene draws into an HDR B10G11R11 image (values above 1). A full-screen tonemap pass reads
  it and renders the tonemapped B10G11R11 image (0 to 1). Its fragment shader varies with the scene
  (`bded5ce917183214` outdoors, `3ac011bd2d772994` outdoors at another time of day,
  `6cb06edf14be1d79` indoors, `96d6efdbfe52605c` in parts of adult Zora's Domain and Lon Lon
  Ranch, and possibly more).
- The game then runs, on that **tonemapped** image: a prepass (`8475d671e6967428`), a heavy
  dispatch (shader `d9ee412f00ce399a`), the network (`42593194bb0b2b32`) and an output pass
  (`da6df3e54c716df2`). The output pass samples one texture, the HDR scene from before the
  tonemap (texture handle at cb0+0xc0, an E5B9G9R9 view of the B10G11R11 memory), and writes the
  B10G11R11 image the rest of the frame uses (texture handle at cb0+0xd0). Keys are native program
  keys.
- The network keeps no history: it is a single-frame spatial smoother (mean |Laplacian| of green
  0.0042 -> 0.0037). The output pass then sharpens hard: 2.2x the input's edge contrast, with
  overshoot up to 1.72 on bright edges. That is the halo users see.
- Only the HUD and a plain copy for presentation follow the output pass.

In the base game (1.0.0), the same tonemap can run below the output resolution. A frame dump in the
Temple of Time showed a 1920x1080 tonemapped scene feeding a 2560x1440 output. The base game's
output shaders use the same constant-buffer handle offsets as the update.

## What No sharpening does

- On 1.1.1, `ShaderCache::ComputeShader` calls `KeplerCompute::BypassOotNetwork`. The output
  pass uses a same-size point copy. The prepass and network are skipped while the replacement works.
  The known intermediate program is also skipped between a skipped prepass and the output copy.
- On 1.0.0, `KeplerCompute::BypassOotBaseNetwork` recognizes the known output programs from
  `DispatchNativeCompute` before translation, which can reject their per-tap gathers. The game
  selects different programs as the internal resolution changes. Match the full native key for
  each candidate that shares the first word. The replacement uses a bilinear copy that can scale
  the tonemap up to the output resolution. The base game's other compute passes still run.
- Both paths use `AccelerateSurfaceCopy` and get the output format, size and block-linear layout
  from the output pass's texture descriptor at cb0+0xd0.

The images are learned at run time, not hard-coded. The copy writes the image the output pass
would write (cb0+0xd0). It reads render target 0 of the latest B10G11R11 draw since the previous
output pass that has the size of the image the output pass samples (cb0+0xc0) and is not the
output (`RasterizerVulkan::TrackOotTonemappedImage`). The scene draws come first, so that is the
tonemap pass whichever shader draws it. The sizes and the output come from the previous output
pass, so the first output pass runs as usual. Builds up to 228 copied render target 0 of the
latest draw with one of three known tonemap fragment shaders. Where the game used another tonemap
shader, that image was no longer drawn, so the 3D froze under a live HUD (#1688). #1875 then
copied the sampled image itself, which is the scene before the tonemap: dark scenes came out
brighter, bright ones darker and with less colour. The prepass and
the network are skipped only while the output pass is being replaced. The update also has output
programs this does not replace (for example the scaled field upscale `e4cda162ff6009e2`, see
docs/native-dlss-replacement.md). A frame that ends in one of them would read the skipped
network's stale result, so a skipped prepass that no replacement followed brings the network back
from the next frame. The log names the first program after the network the first time that
happens. The replacement requires two
B10G11R11 block-linear images, and the tonemapped image must fit the output dimensions (an exact
match on 1.1.1). If no tonemapped image was drawn or the copy fails, dispatch falls back to the
game's shader. The state lives in the rasterizer (`OotNetwork::BypassState`) because the tonemap
pass draws on the 3D engine and the network's passes do not all run on the same compute engine.

NVIDIA DLSS takes precedence: while Ocarina DLSS is enabled ("Use NVIDIA DLSS in place of a
game's DLSS" on, with DLSS available from Streamline), `RasterizerVulkan::OotNetworkBypass` returns
null and No sharpening does nothing, because DLSS replaces the same output pass
(docs/native-dlss-replacement.md). The log says
`Ocarina of Time: No sharpening is off while NVIDIA DLSS is on` once. Once DLSS turns itself off
for the session or its evaluation fails, No sharpening takes over from the next frame.

The game's constants (shader hashes, program keys, the cb0 offset) are in
`src/video_core/oot_network.h`.

## Results (Ryzen 7 9800X3D, RTX 5090, Kokiri Forest, 1x)

| Mode | fps | Image |
|---|---|---|
| Game | ~41 | sharpened, halos |
| No sharpening | ~50 | clean; edges aliased as rendered, pairs with resolution scaling |

## Caveats

- The known shader keys cover base 1.0.0 (main build ID
  `33FA6C2C23848282A77E8B88A0A93F4887C55920`) and update 1.1.1
  (`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). A different revision needs its shaders checked.
- B10G11R11 has little precision in dark tones: without the network's smoothing some banding
  shows in deep shadows.
- Vulkan only: other backends offer no bypass state, so the setting has no effect there.
