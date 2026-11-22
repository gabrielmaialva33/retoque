# Light limiter at any cadence, global and regional

Status: implemented on D3D12 behind `[DlssNr] LightLimiter` (default **0, off**) and a combo under "Light stability";
host-tested (`tests/nr-light-limiter`: the production gate and the shader's own rule over a CPU model of the
dispatches, with mutants). The cadence bytecode (`DlssNr_Cadence_Shader.cso/.h`) was rebuilt with dxc 2026-11-22.
**Not built into a DLL by its author, not run on a GPU or in a game.** Native Vulkan and the driver proxy have no
implementation and are not offered the control.

## The problem

Model cadence's smoothing (`cadence-smoothing.md`) owns a light limiter: the mean light of what the cadence shows may
move by at most 1.5 stops a second while the frame's own light holds still. It only ever ran with `Cadence` above 1
**and** `CadenceSmoothing` on. A default single pass on every frame (`Cadence=1`) had no light stabilization at all,
and the model re-decides its light a little on every frame it runs: A-ENTROPY measured the model's own exposure moving
0.645 to 0.925 on an input that stayed at 0.490 to 0.502 ("lamp breathing"), and janblade measured a still-camera
flicker about ten times higher at the bright end of the model's input range.

## What it does

`[DlssNr] LightLimiter` runs the limiter on the answer of any frame the cadence's own limiter did not hold, in two
forms, into a surface of its own, so nothing the model reads or writes changes:

| Value | Form | Rate |
|---|---|---|
| 0 | off: nothing is allocated, recorded or dispatched | |
| 1 | **global**: the cadence's own rule (`CadenceLimitLight`) over the whole picture's light | 1.5 stops/s while the frame's own light moved by under 0.15 stop |
| 2 | **regional**: the same rule once per 32 pixel tile (`CadenceLimitRegion`), the gain bilinear between tile centres | 0.75 stops/s per tile while that tile's own light moved by under 0.15 stop |

Anything else reads as 0 (a typo never turns on something that changes what is shown).

### The regional band and the detail

Skynizz splits the edit into a regional band (a tent of about 36 px) and the detail, and limits only the band. Here the
split is the gain field. Per tile, three dispatches (`dlssnr_cadence.hlsl`, modes 10 to 12) sum the answer's light, the
proxy's light, the motion's length and a weight (the interface out), hold the tile's `log2(shown / proxy)` to the last
frame's, and apply the tile gains bilinear between centres, as a multiplier on the answer's light. A multiplier that is
smooth at 32 px leaves the contrast of what is finer than a tile in stops exactly as the answer made it
(`tests/nr-light-limiter` holds it at 0.01 stop over a texture): the band moves slowly, the detail is not blurred or
averaged. It is not a low-pass of the edit: the model's own detail decisions are not touched.

A tent is not a box. The gain of a tile falls to half where it meets the neighbour's, so at a tile's very edge the held
light moves at the unlimited rate and in the middle at the step; over a tile, a quarter of the raw swing plus three
quarters of the step. In the synthetic of the suite (a tile's edit swinging 0.2 stop at 2.4 Hz on a still picture) the
worst step of the tile's light falls from 0.050 to 0.022 stop a frame, not to the step (0.0125). That is a property of
the interpolation, not a defect, and it is the reason Skynizz's measured improvement (regional trembling -35%) is the
expectation to keep, not the synthetic's 56%.

### Where it lets go, so a real change is never held back

| Condition (per tile) | Why |
|---|---|
| the tile's own light (the proxy's) moved by 0.15 stop or more since last frame | the picture changed here: a cut, a flash, a door |
| the tile's mean motion is 6 pixels a frame or more (scaled to the working size) | its light follows the content, not the model |
| the model's answer drifted from what was shown by 0.30 to 0.40 stop, over a linear ramp of the hold | a light that really came on is the model's to say; Skynizz releases "smoothly as the step nears 0.35 stop" |
| anything not finite, nothing measurable (black), no state yet | passes as it is |
| the model's history reset, a gap over 250 ms, a size, format, route or mode change, a frame the limiter did not hold | the gate (`DlssNr_LightLimit.h`) starts the state over |

Two calls in one present (a second view) are not limited: one view's light must not hold another's. A frame the
cadence's own limiter held (smoothing on a running cadence) is not limited a second time; the next frame this limiter
runs on starts over.

Numbers not taken from a source: the 32 px tile (Skynizz's is a 12 px tent for his probe and about 36 px for his
limiter), the 0.30/0.40 release band (his 0.35 as a point), the motion threshold scaled to the working size. They are
fixed, not tuned.

## What it is not

- Not the output stabilizer (`output-stabilizer.md`), which holds still pixels still by comparing the model's input.
  This holds the light of a region and lets the pixels move.
- Not temporal filtering of the answer: nothing is blended with an old answer; a gain is applied to the current one
  (`retoque/dlssnr/README.md`, "Temporal filtering ... is a dead end"). The model's own history is untouched.
- Not the "regional band eased with weight 0.95" of Skynizz, which blends the edit with a history of itself. The hold
  here is a rate limit on the gain, with no history of images.

## Where it is

- D3D12 after-upscale, present and bridge routes (everything that goes through `Dispatch`), HDR and tone-mapped frames
  (`passthrough` as the cadence's limiter does), every cadence including 1, before the upscaler too.
- After the cadence's record and fade, so a carried frame still carries the model's own edit; before the down-leg, the
  unpack and the resolve, which read the limited answer (`shownAnswer`).
- One object of the cadence's pass built "light-only" (`DlssNr_Cadence_Dx12(..., InSmoothing = true, InLightOnly = true)`):
  the shown surface (RGBA16F, working size), the 16x16 grid and 4 texel state of the global form, and the tile grid
  (RGBA32F) and state (R32F, 4 texels a tile) of the regional one. It owns none of the cadence's edit, snapshots or
  chain, and its other entries refuse it. Parked like the cadence when the size moves or the key goes off, deleted at
  shutdown. The cost is one RGBA16F texture of the working size and three small dispatches a frame.
- The state is R32F on purpose: a typed UAV load that every adapter has. The grid is only stored to, never loaded.

## Source

Skynizz's anti light pop-in (optiscaler-dlss5 v1.2.1, GPL-3.0, the same licence as this repository): the regional band
split, the gate on the region's own light (0.15 stop), the 0.75 stop/s rate
(https://github.com/Skynizz/optiscaler-dlss5/blob/fc702f8ca2/OptiScaler/shaders/dlssnr/precompile/dlssnr_cache.hlsl#L1526
and https://github.com/Skynizz/optiscaler-dlss5/blob/fc702f8ca2/OptiScaler.ini#L1800), the release as the step nears
0.35 stop and by motion over about 6 px a frame. Skynizz measured regional trembling -35% on a control still view, the
flicker metric 0.57% shipped to 0.40% (Quality) against 0.30% for the game alone, two passes 0.47%. A-ENTROPY's "lamp
breathing" measurement and route G (stabilize only a low-frequency residual and pass the high frequency through) are the
other half of the reasoning (https://github.com/A-ENTROPY/magpie-dlss5-amd, GPL-3.0). No code of either is ported: the
rule, the tile passes and the gate are this fork's, on the global limiter this fork already had.

## How to measure it in a game

On a still view with a lamp, a window or a screen (anything the model re-lights) and `PulseMetric=true`:

1. `LightLimiter=0`, then "Measure the light step" under Compare (`pulse-metric.md`) and "Measure detail" for the
   flicker figure; note the mean step of the "every frame" kind.
2. The same with 1, then 2. The pulse metric is a whole-picture number: the regional form should show its effect in
   "Measure detail"'s flicker figure and by eye (a pulsing lamp), not necessarily in the global light step.
3. A pan, a fast turn, a character crossing the frame, a light switched on: look for a lag in the picture's light after
   a change that is real (the gates above should let it through within a frame), for tile-shaped steps of light (a
   32 px grid) around a moving light, and for the gain being visible at the interface (it should not be: the interface
   is out of the light and the gain).
4. With `CadenceSmoothing` on at `Cadence=3`, the cadence's own limiter holds those frames and this one stands down;
   the frames between are forced ones only.

## Not verified

Everything about the picture. No GPU, no game, no Windows, no D3D12 execution of modes 10 to 12: the DXIL compiles
(dxc 1.x under Wine) and the rule is run by the suite on the host, but the typed UAV loads from R32F, the group-shared
reductions and the new tile dispatch sizes have not run. Whether 32 px, 0.75 stop/s and the 0.30/0.40 release band are right
for this fork's model (Skynizz's were measured on his). Whether the global form at 1.5 stops/s is enough at `Cadence=1`.
Proton is not a D3D12 correctness oracle: native Windows validation is required, as for the cadence's own smoothing.
