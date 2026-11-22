# Exposure governor: a rate limit on the white point the pass applies

Status: implemented on D3D12 behind `[DlssNr] ExposureGovernor` (default **off**) and a checkbox under "White point
from > The game's own exposure". Host-tested only (`tests/nr-exposure-governor`): **not built into a DLL by its author,
not run on a GPU or in a game.** Native Vulkan has no implementation and is not offered the control.

## The problem

`WhitePointSource=1` (the default) divides the frame by `preExposure / exposure * trim`, with the exposure the game
publishes. When a game's exposure hunts (auto-exposure around a bright window, a fade, a lamp switching on, an eye
adaptation that overshoots) the divisor moves with it, the model is shown a differently normalised picture every frame,
and the whole picture pumps. Nothing limited the applied divisor: `DlssNr_Exposure.h` only has a plausibility latch,
which gives up on a game whose exposure is absurd at both ends and never touches one that is merely fast.

DLSS5-NR-Nexus measured the same class of fault from the other side: a per-output estimator alternated by 0.29 stop on
394 of 449 frames; one state per stream, with only the APPLIED divisor rate-limited, removed it. janblade's
TROUBLESHOOTING note "Brightness pumps or flickers on fades" and Skynizz's `AutoFollowsGameExposure` (a second meter
adapting on top of the game's own adaptation pumps) are the same story with a different estimator.

## What it does

The estimate is the game's, fresh every frame, untouched. Only the white point handed to the encode and the resolve is
limited, by `DlssNr::ExposureGovernor::State::Update` (`retoque/dlssnr/DlssNr_ExposureGovernor.h`):

| Rule | Value | Why |
|---|---|---|
| Rising (the picture brightening under the model) | at most **4 stops/s** | the over-range direction: a bright flash must not blow the model's input out for long |
| Falling (the scene relaxing) | at most **0.5 stops/s** | slow adaptation; this is where hunting shows |
| Hysteresis | engage at **0.15 stop**, track until within **0.02** | a target that jitters inside the band does not move the applied value at all |
| Time | the step is `rate x dt`, `dt` clamped to 0.25 s | the speed is per second, not per frame (60, 30 and 144 fps move alike); a hitch cannot pass a whole re-grade |
| Unreadable frame | hold, keep the hysteresis state | a black frame, loading screen or fade-to-black publishes an extreme exposure; the scene after it is followed at the limit, not snapped to |
| The game's reset (or a scene cut the synthesized motion found) | re-prime: take the target at once; wait for the first readable frame if this one is not | a cut says the held value is another scene's |
| A pause over 2 s (menu, load, pass off) | re-prime | the same |
| One state per stream | `g_nr.exposureGovernor`, one for the process | per-output state is what alternated in Nexus |

The zero-guide policy reset (`ZeroGuideReset`) does **not** re-prime: it asks on every frame for the model's sake and
would turn the governor off.

### What "unreadable" means here

Nexus decides it from the frame: too few lattice samples above 1/1024 of the committed median. This path measures no
frame light (the meter is the game's 1x1 exposure texture and nothing else; the old frame meter was removed because it
fed back on its own output). So the signature used is the game's own exposure: the implied white point
(`preExposure / exposure`, before trim) outside `[0.02, 512]`, the range `PlausibilityLatch` already uses and documents
as what "a loading screen or a black frame can push the exposure to" for a moment. One end alone holds; both ends over
a session still latch to paper white as before. A black frame whose exposure stays inside the range is not recognised.

### Why the shader's live read is off while it is on

With a live exposure texture the shader recomputes the white point itself on every frame, with no CPU in the loop
(`WhitePoint()` in `dlssnr.hlsl`). A rate limit needs a state, and putting one in that shader means a new cbuffer
field, a state texture and all three bytecode permutations rebuilt. The governor is therefore on the CPU: while
`ExposureGovernor` is on, `useGameExposure` is 0 and the CPU white point (`ResolveWhitePoint`, from the meter's 3-4
frame readback, held between readings) is the governor's target. The cost is real and said in the help text: a sudden
flash reaches the model a few frames later than with the live read. How much that matters was not measured. Off, nothing changes: the live read, the readback and every byte of the white point are as
before (`tests/nr-exposure-governor` guards that the only write to `whitePoint` in between sits inside the key's
branch).

## Where it is, and where it is not

- D3D12 after-upscale, present and bridge routes (everything that goes through `Dispatch`), source 1, HDR/linear
  buffers (a frame the game already tone mapped has no exposure to govern), exposure not latched, frame not held.
- Not source 0 (a constant) and not source 2: the exposure scan's anchored value is eased by its own rules
  (`exposure-scan-match.md`), and a second limiter on top would only add lag.
- Not Vulkan: that route reads its meter in `DlssNrFeature_Vk.cpp` and has no `useGameExposure`; no control is drawn.
- The stabilizer, the pulse metric and "Measure detail" take `whitePoint` as it is applied, so they see the governed
  value; a white point that moves (even slowly) reads as change to the stabilizer, as before.

## Source and licence

The rule is **DLSS5-NR-Nexus's normalization governor** (williamstosh, MIT, Copyright (c) 2025 Carlos Lopez Jr.),
`shaders/v6_autoscale.cs_5_1.hlsl` and `dlssnr.hpp` at commit `8fabd388e9`
(https://github.com/williamstosh/DLSS5-NR-Nexus/blob/8fabd388e9/shaders/v6_autoscale.cs_5_1.hlsl): asymmetric
wall-clock limits, the 0.15 / 0.02 hysteresis hold, hold on a frame the meter cannot read, re-prime on the game's
Reset, one state per stream. `Licenses/NRNexus_LICENSE.txt` carries their licence. The code is ours: Nexus keeps the
state in a 1x1 UAV beside a 64x36 frame histogram on the GPU; here it is three values on the host beside the game's
exposure, and the "unreadable" test is the exposure's, not the frame's.

Related ideas read, no code taken: janblade's percentile meter excluding black and very bright tiles with 0.5 s / 1.5 s
adaptation (PRs #48, #52, #88, GPL-3.0) and Skynizz's dead band on the automatic white point (`3aed2802`, `979f2441`,
GPL-3.0). Both measure the frame; this does not.

## How to measure it in a game

Pick a place where the exposure hunts (a doorway into daylight, a fade, an eye adaptation overshoot, a lamp), then:

1. `[DlssNr] ExposureGovernor=false`, a still camera, "Measure the light step" under Compare (`PulseMetric`) and
   "Measure detail" while the exposure moves: note the light step and the flicker figure.
2. The same with `ExposureGovernor=true`. The log's `DLSS-NR composition: paper white X` line (rate-limited, one per
   change) is the APPLIED white point; the panel's "Game exposure ... -> white point" readout is the target. They should
   differ only while the target moves faster than 0.5 stops/s down or 4 up, and the log should show fewer, smaller steps.
3. Walk through a cut (a loading screen, a cinematic): the applied value should jump with the game's reset, not slew.
4. Look for what it costs: a fade to bright that takes a moment longer to reach the model's normalisation (the 3-frame
   lag plus the 4 stops/s attack), and a dark room reached from daylight that stays at the daylight divisor for up to
   `stops / 0.5` seconds (a 6 stop change takes 12 s to follow completely; that is the limit's point).

## Not verified

Everything about the picture. No GPU, no game, no Windows. Specifically: that the 0.5 stops/s release is right for
games whose adaptation is slower or faster than the model's tolerance; that 3-4 frames of readback lag matter or do
not; that the plausibility range holds back the black frames of any real game; that a game whose exposure is not
published (source 1 with no texture) is unaffected (the source test says so, the code path is the old one). The defaults
are NR Nexus's own and were not retuned for this fork.
