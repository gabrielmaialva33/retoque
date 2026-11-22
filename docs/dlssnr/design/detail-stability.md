# Detail stability: a temporal filter on the model's edit, with a 3x3 clip

Status: implemented on D3D12 behind `[DlssNr] DetailStability` (default **off**), with `DetailStabilityBlend` (0.15) and
`DetailStabilityHold` (off), and controls under "Light stability". Host-tested (`tests/nr-detail-stability`: the shader's
own rule over synthetic sequences, the gate, 25 mutants). The new shader (`dlssnr_detail_stability.hlsl`) compiles with dxc
2026-11-22 (1.9.2602.17). **Not built into a DLL by its author, not run on a GPU or in a game.** Native Vulkan and the driver
proxy have no implementation and are not offered the control.

## The problem

On foliage, grass, hair and grain the model re-decides fine detail a little every frame, and it boils. DLSS5-NR-Nexus
measured it on panning foliage: NR changed 66% of pixels a frame against 44% for DLSS alone; with this filter at alpha 0.15
every case it measured came below DLSS alone. NeuralScreen (PolyForm Strict: idea only, no code read or used) reports the
same idea with a history weight of 0.75 per 1/60 s and differences above 3 to 8/255 passed as real: pan 0.71 to 0.40,
moving object 1.14 to 0.63, still 0.23 to 0.12.

## How this is not the recorded dead end

`retoque/dlssnr/README.md` records that temporal filtering of the model's answer was measured twice and abandoned: the
model re-decides detail with the framing, so old answers ghost. Both attempts blended old answers with no test of whether
the old answer still describes what is under the pixel. This pass is different in the two places that matter, and is
opt-in and unmeasured here:

1. **The history is clipped to the current 3x3.** The filtered edit may not leave the range the current frame's own edit
   spans in the 3x3 around the pixel. An edit the new frame no longer has anywhere nearby cannot persist: it is replaced at
   once. (`tests/nr-detail-stability`, "the clip".)
2. **The history is thrown away where the input moved under it.** If the reprojected input leaves the current 3x3 range of
   the input by more than a near binary band, the model's answer is shown as it is (a disocclusion, a cut, a flash).
   (`tests/nr-detail-stability`, "the dead end side by side": an object leaves a texture; a plain 0.85/0.15 blend of the old
   answer leaves 0.1 or more of the old edit in the uncovered pixels, this pass shows the new answer.)

What is filtered is the **edit**, `log2((answer + k) / (proxy + k))` per channel, not the answer: a history of the edit moved
with the game's vectors stays attached to the surface it was a decision about, where a history of colours does not.
The model's own history is not touched; this is display only, like the cadence's smoothing and the light limiter.

## What it does

`DsFilter` in `dlssnr_detail_stability_rule.h`, one thread a working pixel, one dispatch a frame, between the cadence's
record and the light limiter (`shownAnswer`):

1. current edit `e` and input signal `s = Y / (1 + Y)` (Rec.709 luminance of the proxy) at the pixel and over its 3x3:
   `lo/hi` of `e` per channel, `slo/shi` of `s`;
2. history `(edit, input)` at `p + motion` (the game's vector scaled to working pixels), bilinear;
3. **reject** where that position is off the picture, any texel that stored nothing is reached, or the reprojected input
   `hs` lies outside `[slo, shi]` by more than `bandLow = 0.02`, completely at `bandHigh = 0.10` (`smoothstep`, near binary,
   in `c / (1 + c)`: A-ENTROPY's note that wide smoothstep weights wobble);
4. `beta = max(alpha, rejection)`; `clipped = clamp(history edit, lo, hi)`; `filtered = clipped + beta * (e - clipped)`;
5. the interface mask blends the edit toward the model's by its alpha; the edit is rounded to the half the history is stored
   in and the answer is made from the rounded edit (`proxy + k` times `2^edit`, minus `k`);
6. stored: the filtered edit and this frame's input.

`alpha` is the weight of the **current** frame (0.15: a history weight of 0.85, Nexus's default); the key accepts 0.05 to 1,
1 is no filter. `k = 1/256`, one step of an 8-bit frame, so black does not turn a mottled grey into a ratio of thousands.

### Repeated input frames (`DetailStabilityHold`)

DLSS5-Feeder's `RESTORE_HOLD_REPEATS` (v2.0.0, MIT): a game at 30 fps shown at 60 Hz presents the same frame twice, and a
model that has a new opinion of it flickers between 0.18 and 0.67 steps where the frame did not change at all; Feeder shows the
repeat exactly as before and measured 0.000. Here, per pixel: with the option on, a motion of exactly zero and an input
exactly the history's (both rounded to half), the history's filtered edit is shown with no clip and no blend, and it is bit for
bit the last frame's because what is stored is what is shown from. This is a **per pixel** test, not Feeder's whole-frame
repeat detection: a pixel of a moving frame whose input did not change at all and whose motion is exactly zero is held too (it
would be anyway under a still camera, by the filter). Where the input differs by even one 8-bit step at a pixel, the
filter decides. Not measured against Feeder's figures.

## Where it runs, and where it does not

It stands down (the key stays on; the history is dropped and a frame comes back from the model alone) before the upscaler
(the jittered render-resolution input reads as change on every pixel), without the game's depth and motion (`GameGuides`;
no vectors to reproject with), with a cadence above 1 (carried frames are not frames of the model's history), with frame
generation active (generated frames are not either), and for a second call in one present (another view). History is
dropped by the model's reset, a gap over 250 ms, a size, format, route or interface-mask change, a frame the pass could not
record, and any of the refusals above.

Cost: three RGBA16F textures of the working size (two histories and the answer, 24 bytes a pixel: 50 MB at 1080p, 200 MB at
4K), one dispatch a frame, nine proxy and answer taps a pixel and four history taps. Parked like the cadence when the size
moves, the key goes off or the stage is before the upscaler; deleted at shutdown.

## Differences from NR Nexus

| Nexus | Here |
|---|---|
| the edit is split into bands (luminance narrow/detail/mid, chroma) | per channel log ratio of the encoded proxy and answer; chroma is in it, no bands |
| the 3x3 input test on the reprojected input in luminance | the same, in `c / (1 + c)`, with a near binary 0.02 to 0.10 band instead of 0.1 to 0.3 smoothstep |
| history carries the edit bands and the input in 12 bytes | `RGBA16F`, edit in `.xyz`, input in `.w` (8 bytes) |
| confidence ramp `smoothstep(0.1, 0.3, change)` | `smoothstep(0.02, 0.10, outside)` |
| optional broad-band "Stabilize" (60 ms) | not taken |
| `NR_EDIT_REUSE_CAPTURE` and the model's reuse | not applicable |

A chroma change at constant luminance does not reject (the input is one channel): a known gap, the cost of an 8 byte history.

## Source and licence

DLSS5-NR-Nexus (williamstosh, MIT, Copyright (c) 2025 Carlos Lopez Jr.), `shaders/look_compose.cs_5_1.hlsl` and `dlssnr.hpp`
at `8fabd388e9`
(https://github.com/williamstosh/DLSS5-NR-Nexus/blob/8fabd388e9/shaders/look_compose.cs_5_1.hlsl#L110): the temporal filter of
the log edit, reprojection with the game's vectors, the 3x3 clip, the rejection against the 3x3 input range, alpha 0.15.
`Licenses/NRNexus_LICENSE.txt` carries their licence. DLSS5-Feeder v2.0.0 (jlrouzies-fr, MIT,
https://github.com/jlrouzies-fr/DLSS5-Feeder/blob/9bd0af3d/shaders/DLSS5_Feed.fx): the exact repeat; its licence is
`Licenses/DLSS5-Feeder_LICENSE.txt`. A-ENTROPY (GPL-3.0, magpie-dlss5-amd, `20260914-dlssnr-temporal-stabilization-routes.md`):
near binary bands judged in `c / (1 + c)`, not wide smoothstep weights. Skynizz (`CacheNoiseAware`, GPL-3.0): comparing against
the 3x3 range, not per pixel, after per pixel tests flickered shadows at half rate under path tracing; idea only. NeuralScreen
(PolyForm Strict): idea only, no code. The code is this fork's.

## How to measure it in a game

Foliage or grass, `PulseMetric` not needed:

1. A still camera, "Measure detail" under Compare with `DetailStability=false`, then `true`: the flicker figure should fall,
   the band detail should not.
2. The same panning slowly, and with a character crossing the frame: Nexus's criterion is that NR must change fewer pixels a
   frame than DLSS alone. Look for ghosting behind moving things and around uncovered areas (the failure the dead end had),
   for a trail on a fast turn and for a softening of fine detail against `DetailStabilityBlend=1` (no filter).
3. A 30 fps game at 60 Hz: `DetailStabilityHold` on and off, a still camera, flicker figure and by eye.
4. A cut and a menu: the first frame after should be the model's, not a blend with the scene before.

## Not verified

Everything about the picture. No GPU, no game, no Windows, no D3D12 execution of the new pass (the DXIL compiles; the half
rounding with `f16tof32(f32tof16())`, the 3x3 loops and the bilinear history taps have not run). That alpha 0.15, the 0.02 to
0.10 band and a luminance-only input are right for this model and these games; that the rejection catches a disocclusion
under sub-pixel jitter; that the clip is enough against the ghosting that ended the earlier attempts. The README's dead end
stands until a measurement says otherwise: this is a new, opt-in experiment, not a reversal.
