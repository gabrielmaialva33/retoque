# Half-rate bilateral look model (offline experiment)

2026-10-29. Experimental design; no production implementation or configuration change.

## Question and scope

Can a local affine colour transform learned from one neural-rendered frame reproduce
the next neural output using only the current input, without motion guides? On even
frame IDs fit a bilateral grid; on odd IDs apply it to the current input. First frames
and explicit resets must run the model. Compare with reusing the previous output.

The reference is MIT-licensed RTX40MFG-Unlock, commit `b97deee` (v1.7.0),
`source/native/dlss5_policy.h` and `source/native/dlss5.cpp`. This experiment independently
implements its statistical model in NumPy; it does not copy its shader. Attribution
and any applicable licence notice must accompany a future port.

The reference's complete Boost path needs motion vectors: it reprojects both grid
coordinates and a held residual, attenuates residuals using a luminance disagreement
test, and composes motion for the next network evaluation. It explicitly disables
Boost at Present. The experiment deliberately tests the simpler, guide-free grid at
the current pixel, with no held residual. It is not a reproduction of full Boost.

### Exact reference model and deliberate differences

At the pinned commit, the cell width is `16 * max(1, round(height / 1080))`,
not invariably 16 pixels. Grid dimensions round up; 640x360 therefore uses 40x23
cells and 8 luminance bands. RGB luminance weights are (0.2126, 0.7152, 0.0722).
Clamp luminance to [0,1], multiply by 7, and splat each pixel to its two adjacent
bands with triangular weights. Each pixel belongs to one spatial cell during fit.
The donor accumulates 22 weighted moments, then blurs **moments**, not fitted
coefficients, across neighbouring spatial cells with a separable [1,2,1] kernel.
Outside neighbours are omitted; there is no blur across luminance bands.

For weighted means `mx,my`, covariances `Cxx,Cxy`, and row-vector convention:

```text
A = solve(Cxx + 0.005 I, Cxy + 0.005 I)
b = my - mx A
prediction = current_encoded_RGB A + b
```

This is ridge regression toward identity, with a free mean offset. Empty bins
(weight <= 0.001) and failed determinant checks (<= 1e-12) keep pure identity.
The Python version stores all nine Cxx entries instead of packing six, and uses
float64 solves instead of GPU float32 reductions/cofactor inversion. At apply,
spatial coordinates are pixel-centre/cell - 0.5, clamped to the grid. Interpolate
the eight spatial/luminance corner transforms and apply to the current colour.

The donor stores residual `network_output - fitted_look` plus input luminance.
On a skipped evaluation it applies the old grid at the motion-reprojected position
and adds the reprojected residual, weighted by the largest luminance disagreement
in a 3x3 neighbourhood. Its strict/normal/loose full-keep to zero-keep thresholds
are 0.01–0.03 / 0.02–0.06 / 0.04–0.12. Out-of-bounds correspondence suppresses
the residual and uses the current position. There is no temporal blending of
successive fitted grids. An explicit `frame.reset` forces a network frame; this
is not a content-based scene-cut detector. Resource changes also invalidate history.

Sources: [pinned policy](https://github.com/dashdogy/RTX40MFG-Unlock/blob/b97deee/source/native/dlss5_policy.h#L317),
[fit/apply shader](https://github.com/dashdogy/RTX40MFG-Unlock/blob/b97deee/source/native/dlss5.cpp#L510),
[Present refusal](https://github.com/dashdogy/RTX40MFG-Unlock/blob/b97deee/source/native/dlss5.cpp#L1234).
[Chen et al., Bilateral Guided Upsampling (2016)](https://research.google/pubs/bilateral-guided-upsampling/)
fits affine transforms in bilateral space to approximate an image operator. Its
spatial upsampling result does not establish temporal reuse quality for NR.

## Pipeline proposal

Keep the existing encode -> NGX feature 18 -> resolve boundaries. Fit encoded model
input RGB to encoded model output RGB, at the model's working resolution. On skipped
frames encode the current game input, apply the last grid into the model-output slot,
then run the existing resolve against the current original and proxy. Do not fit raw
game-linear colour to the final composed image and call that the neural look model.

WorkingScale, exposure/white point, reversible encoding, passthrough and transfer mode
change these domains. Invalidate the grid on their changes, resize, route/model
recreation and reset. A future implementation needs its own lifecycle and timing
review; any constants must append to both DlssNrConstants and the HLSL cbuffer.

After-upscale can obtain the normal model inputs and guides. Present without guides
could apply a grid, but this says nothing about the quality of the network frames
themselves without real guides. RTX 20/30 could benefit if saved model time exceeds
fit/apply cost; offline CPU timings cannot establish that saving or its ratio to a 4090.

This addresses a different admission constraint from [model cadence](model-cadence.md):
that path carries a prior edit using motion and depth, and refuses Present without
captured real guides. [Adaptive cadence](cadence-adaptive.md) changes scheduling;
it does not replace the carried representation. The [2026-10-28 adaptive2 analysis](../../superpowers/reviews/2026-10-28-temporal-cadence-analysis.md)
rejected adaptive2 on both trajectories. Nothing in this experiment reverses that
decision or compares directly against those numbers: the diagnostic colour domain
and full-rate oracle history are different.

Potential cell seams are softened by spatial interpolation, but an object crossing
a cell can still acquire its neighbour's old transform. Luminance bins distinguish
brightness, not material identity or semantics. Missing high-frequency neural detail
can create a visible network/prediction alternation even on a static frame. HDR
highlights can map into the same compressed bands, and a small near-ceiling proxy
error can become large under a reversible inverse. Alpha, HUD masks, compare modes,
passthrough and signed wide-gamut contributions must remain owned by the existing
resolve; the grid must never overwrite the current original. Transfer 2 has a separate
carrier/upscaler path and is outside this prototype. No proposed UI or INI key yet.

## Measurement and acceptance boundaries

Use the retained forward and reverse 640x360 synthetic temporal replays, with real
network outputs every scored frame, 80 warmup and 400 scored frames each. Existing captures expose the NR boundary and
composed output. Diagnostic DebugView 1/2, already implemented, expose decoded proxy
and decoded model RGB at DebugScale 1. Re-encoding these yields an approximation to
the network buffers, with an additional FP16 readback quantization. The debug
decode also clamps to [0,1], so unbounded raw model output cannot be recovered.
Measure in the decoded diagnostic domain, separately from production
composition. No game capture or production shader changes are required.

Compare 16px/8 bands, 32px/8 bands and 16px/16 bands, ridge 0.005. These choices
were fixed before reverse analysis; there was no tuning against either result.
Reuse the replay
metric implementation for golden MAE and paired pulse distance against an independent
every-frame control, including per-phase fast motion and cut. Quantify A/A noise;
these descriptive numbers are not a calibrated acceptance PASS. Reset frames copied
from the oracle must be distinguished from a deliberately unsafe stale-grid cut.

Before any port: improve on plain hold across both motion directions; retain quality
against every-frame within a separately calibrated envelope; visually inspect edges,
cell seams, newly revealed objects, highlights and UI; test negative/HDR/extreme exposure
and tone changes; measure fit/apply and memory on RTX 20/30; validate actual skipped
network history and cut detection; validate D3D12 and Vulkan lifecycle rules and native
Windows. Neural high-frequency detail absent from current input cannot be reconstructed
by a colour-only affine grid. Flicker, unsupported luminance bins and highlight
extrapolation remain risks even if aggregate MAE improves.

### Protocol

Three fresh diagnostic processes per trajectory: DebugView 1 input, and DebugView 2
every-a/every-b outputs. Each of the six trials completed all 400 scored model
evaluations. Both A/A pairs were bit-identical for all 480 frames, including warmup.
This is two controls, not the five-repeat calibrated envelope required by the replay
judge; functional replay PASS and A/A equality do not constitute a fidelity PASS.

Fit uses every-a on even frame IDs, apply uses the immediately following input on
odd IDs. First usable frames and reset force the oracle and refit. Each trajectory
therefore has 201 oracle and 199 predicted scored frames. The cut at ID 361 is odd
but forced. Warmup is processed to retain frame 80 for the first scored transition.
An unscored skip breaks history, never becomes a predicted model frame.

Golden MAE is mean absolute RGB error against every-a, first per frame and then
across the phase. Units are decoded, normalized model colour. Paired pulse distance
is `abs(candidate pulse - every-b pulse)` at the same frame, in stops, using
`numpy_metrics.frame_metrics` and `metrics.percentile` unchanged. The current proxy
is the common source for all methods. Quantiles include both oracle and predicted
transitions; the aggregate has 399 pulse pairs because reset has no pulse.
Predicted-only statistics are also retained in the JSON. `hold` copies the prior
output without motion: it is the requested simple baseline, not production cadence's
reprojected residual or adaptive scheduling. The centre region is reported as well
as the full frame, and cannot be replaced by full-frame averages for acceptance.

Oracle frames use every-a's full-rate answer exactly, including its full-rate hidden
state. A real half-rate NGX invocation could change all subsequent answers. This is
an offline representation test, not an emulation of actual skipped network history.
There is no GPU performance claim: NumPy processing times include fitting, metrics
and input validation and have no connection to model milliseconds or game FPS.

### Evidence and reproduction

Source checkout: `c862bd2e`, isolated branch `codex/look-model-half-rate-20261029`.
The retained DLL logs build **abad9704**, SHA-256
`4d48c575e880049ca56dcc6a75d27d88be842621a10a5c66c0e6e820e31a805c`;
harness `36f2f2152c787d1bf2d93955d8b3770173c254b0d13d2226b643d36019b3535c`.
The old forwarder link pointed into a removed worktree, so all new diagnostics used
the same available forwarder, `68f78e20ebf2e150647c66c584631a7ddcb86c4346566260ae0ecbaba178c8ad`.
The new measurements are compared only with their fresh matching controls, not with
old diagnostic outputs. The run manifest's producer commit identifies orchestration;
it does not mean the reused DLL was built at that commit.

Capture manifest hashes:

- forward: `f17fea2bac406cb0821d98225f720920240d48e45e0a30b40fe9185ed1497c57`
- reverse: `42e7b2549b34d08b4b61e057eb244108143f02d08f4bc2c3041b64d7542b0dfb`

Local retained inputs are below `~/.cache/retoque-wt/evidence/`: forward is
`qualification-20261028/tests/dlssnr-loopback/artifacts/replay-quality-run/corpus/capture.json`;
reverse is `temporal-analysis-20261028/tests/dlssnr-loopback/artifacts/replay-quality-reverse-run/corpus/capture.json`.
New artifacts are inside this worktree's `tests/dlssnr-loopback/artifacts/`:
`replay-run/runs/` for forward diagnostics, `look-diagnostic-reverse/replay-run/runs/`
for reverse, and `look-forward/` / `look-reverse/` for NumPy results and frame records.
An independent retained copy is under `~/.cache/retoque-wt/evidence/look-model-20261029/`:
`diagnostics-forward/`, `diagnostics-reverse/`, `look-forward/` and `look-reverse/`.
It includes raw outputs, manifests, logs and reused harness/DLL/forwarder binaries;
it excludes the Proton prefix and model binaries. Relative output-plane paths remain
valid without the scratch worktree.
Their manifest identities and compact results are committed in
[look-model-20261029-results.json](../../../tools/replay/experiments/look-model-20261029-results.json).
Scripts and exact repeatable command templates are in the
[experiment README](../../../tools/replay/experiments/look-model-README.md).

No game was opened. The six GPU diagnostic replays used the existing prefix lease
and isolated Proton artifacts; no compiler, build or full Wine tier was run.
The NumPy jobs ran with one BLAS thread in 8G/10G memory-capped scopes (swap cap 2G).
Input/output plane hashes were checked; 960 frames' depth/motion conventions and
unit exposure were independently validated. Four raw float planes matched the
strict stdlib reader exactly. The prototype's 12 analytic cases passed (identity,
independent weighted least-squares, empty bins, transfers, constant/scalar slicing),
as did replay metrics **47 tests** (including NumPy/scalar parity), replay format
**9 tests**, and the existing NR A/B suite. Production C++/HLSL and submodules
were not changed.

The captures have HDR flags but do not stress HDR range: all original RGB values
are below 1, with only tiny negative ringing. A CPU sweep of all FP16 values in
[0,1], decoded and quantized to FP16 then re-encoded, has maximum encoded error
0.00015997. This bounds that idealized transfer round trip, not shader arithmetic,
out-of-range clipping or the effect on regression coefficients. Exact raw NGX
input/output capture remains a gate before a production port.

## Results

Tables below report the full-frame region; the JSON also retains centre-region
and predicted-only results. Cut MAE zero means the oracle was forced, not that a
stale transform handled a new scene. Cut pulse is unavailable, never zero-filled.

### Whole scored sequence and fast motion

| Trajectory | Method | Whole MAE | Whole pulse p95 | Fast MAE | Fast pulse p95 |
|---|---|---:|---:|---:|---:|
| forward | hold | 0.000247054 | 0.02008182 | 0.001533555 | 0.04243049 |
| forward | look16x8 | 0.000198780 | 0.00613212 | 0.000525039 | 0.01699468 |
| forward | look32x8 | 0.000240377 | 0.00635141 | 0.000596501 | 0.01616885 |
| forward | look16x16 | 0.000191767 | 0.00578422 | 0.000525873 | 0.01628765 |
| reverse | hold | 0.000264007 | 0.01472427 | 0.001186038 | 0.03267573 |
| reverse | look16x8 | 0.000261687 | 0.00559790 | 0.000399987 | 0.01168384 |
| reverse | look32x8 | 0.000347523 | 0.00559144 | 0.000456659 | 0.01154057 |
| reverse | look16x16 | 0.000250720 | 0.00564531 | 0.000401167 | 0.01190356 |

### Per phase: reference 16px/8-band grid versus simple hold

| Trajectory / phase | Frames | Hold MAE | Look MAE | Hold pulse p95 | Look pulse p95 |
|---|---:|---:|---:|---:|---:|
| forward / steady | 80 | 0.000095711 | 0.000170607 | 0.00552502 | 0.00557538 |
| forward / motion | 80 | 0.000157207 | 0.000183888 | 0.00790962 | 0.00480440 |
| forward / recovery | 80 | 0.000103866 | 0.000178913 | 0.00535571 | 0.00551585 |
| forward / fast-motion | 40 | 0.001533555 | 0.000525039 | 0.04243049 | 0.01699468 |
| forward / cut | 1 | 0.000000000 | 0.000000000 | unavailable | unavailable |
| forward / post-cut-recovery | 119 | 0.000075098 | 0.000133092 | 0.00506463 | 0.00504112 |
| reverse / steady | 80 | 0.000116452 | 0.000264527 | 0.00408736 | 0.00408325 |
| reverse / motion | 80 | 0.000381213 | 0.000354155 | 0.01004722 | 0.00558831 |
| reverse / recovery | 80 | 0.000117640 | 0.000291785 | 0.00355281 | 0.00385460 |
| reverse / fast-motion | 40 | 0.001186038 | 0.000399987 | 0.03267573 | 0.01168384 |
| reverse / cut | 1 | 0.000000000 | 0.000000000 | unavailable | unavailable |
| reverse / post-cut-recovery | 119 | 0.000075098 | 0.000133092 | 0.00506463 | 0.00504112 |

Both every-b controls have zero golden MAE and zero available paired pulse distance.
Predicted-only full-frame MAE (199 frames; the above includes oracle frames):

| Trajectory | Hold | 16x8 | 32x8 | 16x16 |
|---|---:|---:|---:|---:|
| forward | 0.000496591 | 0.000399558 | 0.000483170 | 0.000385462 |
| reverse | 0.000530666 | 0.000526004 | 0.000698539 | 0.000503960 |

### Unsafe cut and training reconstruction

Ignoring reset and applying frame 360's transform/output at cut 361 gives:

| Method | Forward cut MAE | Reverse cut MAE |
|---|---:|---:|
| hold | 0.038030372 | 0.004354194 |
| look16x8 | 0.004114164 | 0.001940825 |
| look32x8 | 0.004156709 | 0.001888863 |
| look16x16 | 0.004194819 | 0.001790768 |

Even on the very frame used to fit the grid, mean decoded reconstruction error is:

| Grid | Forward training MAE | Reverse training MAE |
|---|---:|---:|
| look16x8 | 0.000314931 | 0.000437588 |
| look32x8 | 0.000419187 | 0.000629695 |
| look16x16 | 0.000295458 | 0.000410322 |

That error demonstrates a representation loss before any one-frame staleness. It
does not isolate texture loss from spatially varying colour, bin interpolation or ridge bias.

## Recommendation

**Do not port this guide-free grid-only variant yet.** Continue only as a bounded
offline investigation of detail preservation and rejection. The current-input approach
helps fast motion in both directions, but a whole-sequence number hides substantial
regressions on stable and recovery phases.

- forward: fast-motion MAE -65.8%, fast pulse p95 -59.9%; steady MAE +78.3%, recovery MAE +72.3%; whole MAE -19.5%.
- reverse: fast-motion MAE -66.3%, fast pulse p95 -64.2%; steady MAE +127.2%, recovery MAE +148.0%; whole MAE -0.9%.

16 bands slightly reduce whole-sequence MAE, but do not remove the stable/recovery
regression. A 32px cell is worse on whole-sequence MAE, including worse than hold on
the reverse trajectory. No variant satisfies the proposed cross-phase improvement
gate. The donor's residual mechanism is a plausible reason its full design retains
more detail, but importing it also imports motion dependence and does not solve the
Present-without-guides objective.

The next experiment needs (1) exact raw network I/O or a validated equivalence to
these diagnostics, (2) a guide-free treatment of missing detail and stale local looks
that wins stable/recovery as well as motion, and (3) content-cut/exposure rejection
that works when no engine reset is available. Only then test actual skipped model
history, complete resolve/HDR behavior, a real game's visual sequences and fit/apply
GPU costs on RTX 20/30. Cell-boundary artefacts, neural-detail alternation, out-of-band
highlights, UI and arbitrary scene changes remain open. These results do not establish
an FPS gain, Present-route quality, or suitability for deployment.
