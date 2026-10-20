# Adaptive model cadence

## Measurement first

Baseline: e5f592de; RTX 4090, Proton Experimental, model 310.8.2.0. The moving
loopback now optionally presents through the production DXGI factory: without
presents the existing scheduler refuses cadence, so the old fixture cannot measure
it. No renderer change was needed. ABBA trials retain 16 measured frames after 64
warmup frames. GPU timestamps bracket public EvaluateFeature (DLAA plus NR), not
only the neural model. Raw captures, logs and provenance are kept under the
worktree's ignored `x64/cadence-measurements/` directory.

Measured results (milliseconds, on-a; repeat on-b retained):

| Cadence | Smoothing | GPU p50 | GPU p95 | Mean compensated edit residual |
|---|---|---:|---:|---:|
| 1 | off | 2.810880 | 2.897920 | 0.00058765 |
| 1 | on | 2.812928 | 2.887680 | 0.00062451 |
| 2 | off | 0.359424 | 2.999296 | 0.00041900 |
| 2 | on | 0.370688 | 3.027968 | 0.00039453 |
| 3 | off | 0.343040 | 3.125248 | 0.00040290 |
| 3 | on | 0.355328 | 3.138560 | 0.00032034 |
| 4 | off | 0.342016 | 3.045376 | 0.00039403 |
| 4 | on | 0.358400 | 3.121152 | 0.00031901 |

All eight ABBA cases passed the fixture checks; logs confirm actual carried frames
for intervals 2–4. The residual is descriptive, in linear fixture RGB, with
geometric reprojection and disocclusion rejection. It is not an image-quality score. Repeating cadence 1 already changes its mean by
about 6%; do not interpret smaller differences as improvement. Timing is bimodal
under cadence, so median savings do not imply lower p95 or smoother presentation.
The fixture's foreground motion p90 is 0.598–0.611 pixels per frame normalized to
1080 lines. It is mild motion, not a camera whip or a static scene.

## Contract

`CadenceAdaptive=auto` means false. With false, the original scheduling inputs,
resource allocation, existing shader modes and dispatch sequence stay unchanged. `Cadence`
remains the maximum interval (2–4); at 1 adaptive does nothing. Existing route,
frame-generation, guide, hold, capture and replace-curve refusals remain binding.
There is no new GPU-time target: measuring total Evaluate cost does not establish
a useful universal model budget. This first admission policy prioritizes motion
safety over reaching the configured maximum interval.

Use the ideas of janblade's MotionGuard (rejection suspends reuse, calm must persist)
and Skynizz's smart cadence (shorter intervals while moving), implemented locally.
Do not copy their code or their performance claims. References:
- `janblade/OptiScaler-F5-DLSSNR-Multipass`, `DlssNrDetailReuse.h`, MotionGuard.
- `https://github.com/Skynizz/optiscaler-dlss5/releases/tag/v1.2.0`.

## Admission and feedback

An opt-in, sparse 16x16 GPU probe measures current guide motion and the actual
carry rule's rejection (1 - acceptance), before history is overwritten. Report
p90 motion in pixels/frame at 1080 lines; exclude invalid guides and interface
pixels, and do not dilute foreground motion with far-plane background. Mean
rejection uses the same sampled valid geometry. Too few samples or nonfinite
values refuse reuse. These are sampled estimates, not full-frame coverage.

Initial conservative thresholds are explicit policy, not measured universal
quality limits: at >=1 px/frame or >=15% rejection, interval 1; between 0.25 and
1 px/frame, at most interval 2; below 0.25 and below the rejection threshold,
allow the configured interval only after 300 ms of fresh calm samples. Shortening
is immediate on receipt of a sample; lengthening waits for calm. Missing, stale
(250 ms or more than four evaluations old), replayed or unproven samples force 1.
Age counts the pass's own evaluations, not presents: under frame generation
several presents go by per evaluation, and a probe answered after the game's
Reset (often two or three evaluations later) would otherwise always be stale.
GPU feedback is delayed: a newly exposed object can affect one frame before the
CPU sees it. No synchronous GPU wait is added to admission.

Effective interval 1 still records the latest model edit and probes subsequent
frames, so calm can recover without toggling the setting. On suspension smoothing
does not fade a stale edit back in. Existing cumulative motion is handed to the
model when returning after carried frames. Resets, signature changes, duplicate
presents and refused routes invalidate feedback and restart calm admission.

## Resource lifetime

Adaptive requires the existing tracked-recording path from its first NR recording.
Enabling it after legacy untracked work cannot safely upgrade resource lifetime:
keep interval 1 and require restart for adaptive admission. Four lazy probe slots
each own a separate descriptor heap, 256-byte constant buffer, grid and readback
allocation; they never consume another slot in the legacy cadence dispatch ring.

CPU readback requires the exact recording to be sealed by Reset, executed once,
and completed on its queue, using the Submission certificate. Slot reuse requires
that certificate's reusable flag **or Settled**, so cancelled or safely orphaned
recordings do not strand a slot; uncertified samples are discarded. In non-strict
mode Settled also permits completed/unsealed recordings after the replay window.
An occupied, unsettled slot is never overwritten. Retirement uses the tracked NR
generation's usage; device loss abandons pending resources. All CPU admission state
remains under g_nrMutex. Admission follows chain-in-flight's bounded overlap once
one-queue affinity is established, with conservative serial/strict fallbacks;
it does not wait for every previous list's Reset before admitting another frame.

## Validation plan

Host tests exercise threshold boundaries, stale/invalid samples, repeated samples,
calm hysteresis, shrink/recovery, reset and default-off scheduler equivalence.
Source guards cover config round-trip, cbuffer order and fenced probe ownership.
The original shader change rebuilt cadence DXIL. The probe-ring correction changes
no shader source or cbuffer: run invariants and all host suites, clang-format 22,
Release under the shared lock, and diff --check. Repeat the moving fixture with adaptive
and explicitly disabled adaptive; compare default-off captures with baseline.
No game deployment. Native Windows, real camera cuts, HDR/game quality and FG
pacing remain unvalidated by this synthetic fixture.

## Implementation validation (2026-10-20)

The adaptive run at maximum 4 with smoothing admitted interval 2 after fresh calm
feedback. Evaluate GPU p50/p95 was 0.401408/3.133440 ms (repeat:
0.366592/3.207168); mean compensated residual 0.00040763. Explicitly disabled,
fixed 4 with smoothing gave 0.338944/2.921472 ms (repeat: 0.347136/3.106816),
residual 0.00030306. This mild-motion fixture does **not** show a residual benefit
from adaptive admission. It establishes execution and measured tradeoffs, not a
quality recommendation or universally calibrated thresholds. It also confirms
that cadence's tail cost remains; no FPS/pacing claim follows from the median.

The host suite passed all 61 suites. An additional compiled comparison against
`e5f592de` checked 100,000 decisions with adaptive left at its default: all decision
fields, including float fade-weight bits, matched. Default-off skips probe
allocation/dispatch/readback and leaves the legacy tracking opt-in unchanged.
The model itself is not deterministic across fresh runs, so separate-run NR
captures are not a bitwise proof of the whole pipeline. Fixed-cadence GPU controls
and provenance are retained alongside the adaptive trial.

The existing cadence pass is D3D12-only; its DXIL/header were regenerated. No
Vulkan cadence shader exists, and no native Vulkan option is exposed. The main
composition shader/cbuffer is unchanged. ProbeHistory is appended only to the
cadence pass's ordered C++/HLSL constants, whose CBV remains 256 bytes.

Once enabled, tracked resource lifetime remains in force for the session even if
adaptive admission is disabled again; protection cannot safely be downgraded.
Default-off equivalence refers to starting with adaptive disabled.

## Probe pipeline correction (2026-10-20, before implementation)

The single pending probe cannot sustain feedback with a three-evaluation Reset
latency: each incoming sample is three evaluations old, but the previous one is
six old. Testing the previous capture against the current evaluation repeatedly
restarts calm. Four independently owned slots cover delays of 2, 3 and 4 evaluations:
poll first, then record into a free slot. Each slot owns its grid, readback, 256-byte
constant buffer, descriptor heap, Usage, sample metadata and ignored/pending flags.
No busy slot is overwritten; if none is free, skip the probe and retain the normal
bounded stale-feedback fallback. Allocation remains lazy and adaptive-only.

Polling checks every pending slot. A reusable certificate or Settled usage permits
slot reuse; only an accepted certificate permits CPU readback. Cancelled, orphaned
or aged-complete uncertifiable samples are returned invalid, never used as calm
evidence. Returned samples are sorted by capture evaluation; the controller ignores
samples at or below the last observed capture, including completions arriving in a
later poll out of order. Invalidation marks every pending slot ignored without
releasing any GPU ownership. Retirement still follows the tracked NR generation.

Continuity compares consecutive capture IDs and capture times (positive evaluation
gap <=4, nonnegative time gap <250 ms), not the previous sample's age at receipt.
The incoming and newest retained samples must still be fresh at the current
evaluation/time. Invalid feedback interrupts calm; fresh calm after it can recover.
A motion/rejection sample shortens immediately on receipt, including within a
batch of completions. Four slots are a bounded latency allowance, not a guarantee
against arbitrarily delayed Reset. No cbuffer, shader math or bytecode change is
needed: every slot uses the existing mode 9 and its existing descriptor layout.

This uses chain-in-flight bounded admission rather than serializing every NR
recording. Settled allows cancellation and completed orphan reuse in either mode,
and completed/unsealed reuse after AgeOutTicks only outside replay-strict mode.
Unknown or outstanding GPU work is never made reusable by age alone. The documented
late-replay and cross-queue limits of chain-in-flight still apply.
