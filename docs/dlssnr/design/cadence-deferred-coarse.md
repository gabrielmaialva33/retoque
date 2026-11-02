# Deferred cadence coarse reduction: RTX 4090 measurements

Date: 2026-11-01. Status: accepted for local use with the measured adaptive configuration.
Source: uncommitted `perf/rtx4090-cadence-20261101` based on `73fad086`.

The change removes coarse-history reductions that would be overwritten before use.
In the measured adaptive configuration, the median GPU time outside the model on
model frames fell by about **0.04 ms**. Replay output and decisions were identical
for all exercised variants. **An overall FPS improvement was not demonstrated.**
The first carried frame becomes more expensive; fixed Cadence=2 showed no net
improvement in this single measured pair.

Design and implementation: [spec](../../superpowers/specs/2026-11-01-rtx4090-cadence-design.md),
[execution plan](../../superpowers/plans/2026-11-01-rtx4090-cadence.md),
[source review](../../superpowers/reviews/2026-11-01-rtx4090-cadence-review.md).

## What changed

`Record` still stores the same residual, proxy and depth history. The unchanged
coarse reduction now runs immediately before the first `Carry` that consumes
that history, selected by the existing `InChainStart` state. Later carries and
Fade reuse it. Repeated model frames no longer build an unused coarse texture.

The adaptive probe calls the extracted `CadenceAccept` rule and binds null
residual/coarse SRVs. Carry retains its original acceptance and edit mathematics.
The probe dependency change is not independently claimed as a speedup: shader
dead-code elimination may already have removed its unused calculations.

Scheduling, thresholds, model settings, history quantization, resource lifetime,
barriers and the 112-slot descriptor ring are unchanged.

## Identity and conditions

The installed baseline announces `175bc188 / 20261101_130352`. The candidate
announces `73fad086 / 20261101_153320`. Comparing those source commits shows
only two documentation changes (`docs/capabilities.md` and the dated
agent-driven game smoke review), so their production source is equivalent before
the isolated optimization.

| Artifact | SHA-256 |
|---|---|
| Baseline OptiScaler | `15a8267f15c8b186bf704fb125f2f31f8f2dbf7c229a625ec7673cbec5940848` |
| Candidate OptiScaler | `a445fe1c53d13bf030606225b252763472eacafd3d8e6086f09340d8e55a1700` |
| Forwarder used in **both** test bundles | `9a75ada29a4be186d9aeb19ce967606fe28f6de8e72fac914ba6171c8715cd46` |
| Game-owned NR model, 310.9.16.0 | `cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed` |
| Candidate cadence CSO | `bb4f8aa0b1e74dd671533585402c234b9fee793fa6f7ca4d01ff2f1fa04ba4f6` |
| Original game INI | `770559fba4090f8c7f0c6439bd09901fc7e7c4b46a004025dd3c3f1fdb489e8b` |

The newly built forwarder was deliberately not varied in this comparison.
The candidate replay records producer
`73fad086eef72bb2e07845d751c9d6d2d514425c+12c69e8240eed4c0`;
the suffix is harness provenance, not a new Git commit. Source and generated
artifacts were frozen before replay and game measurement.

Environment: RTX 4090, driver 615.78.08, Proton Experimental, Cyberpunk 2077 2.5,
3440x1440 fullscreen, DLAA, Ray Reconstruction and path tracing,
`ApplyAfterRR=true`, `RRWorkingScale=1.0`. Frame generation and the existing
`CadenceWithFrameGen=true` setting were retained. Main comparison:
`Cadence=4`, `CadenceAdaptive=true`, `CadenceSmoothing=true`.
Only the diagnostic GPU timing setting was enabled for the experiments.
VSync and the game FPS cap were off. VRR, power limits, clocks and driver settings
were not changed.

## Same-input replay

The held capture contains 93 frames: 40 warmup and 53 scored frames, with resets
at frames 1 and 41. Capture SHA-256:
`32eda6dea8694c2a5543be1a26ed364fab1a0cf1db88883adb8f5cdd83c10d94`.

| Variant | Baseline/candidate decisions | Baseline/candidate output bytes |
|---|---|---|
| NR off | Identical, 93/93 | Identical, 93/93 |
| Every-frame model | Identical, 93/93 | Identical, 93/93 |
| Fixed cadence 2, smoothing | Identical, 93/93 | Identical, 93/93 |
| Fixed cadence 4, smoothing | Identical, 93/93 | Identical, 93/93 |
| Adaptive cadence 4, smoothing | Identical, 93/93 | Identical, 93/93 |

No scored frame was skipped. Of the 53 scored frames, fixed C2 exercised 26 first
carries; fixed C4 exercised 13 first carries and 26 later carries. Their intervening
model frames exercise Fade. Adaptive replay had 53 model frames, so real GPU
coverage of adaptive recovery into sustained carries is limited. Host regressions
cover acceptance populations, odd edges, history replacement and partial-failure
recovery; they do not replace driver execution.

A fresh baseline process repeated every-frame and fixed C4 after the game runs.
Both matched the original baseline decisions and bytes on all 93 frames. This
bounded A/A identity check was performed after A/B, a documented order adjustment
to avoid extra GPU work during game measurement. The optional multi-repeat
numerical/perceptual calibration was not run. This establishes equality on the
exercised inputs, not universal image-quality qualification.

Replay timings were noisy even for the same DLL: the baseline every-frame mean
changed from 11.390 to 10.210 ms and C4 from 16.501 to 13.126 ms. They are not used
as performance evidence.

## Main real-game comparison

Both builds received a warmup, followed by **old1, new1, new2, old2**. The same
receipt installer swapped the frozen bundles only while the game was closed.
`GpuTimingInterval=31` was identical in both arms. No build/compiler job ran
during these measurements.

GPU timestamps bracket the NR dispatch; `outside_model_ms` is the dispatch
scope minus its model span. It is not total game frame time. The game CSV supplies
intervals between rendered frames; those are not pure GPU execution time either.
The game reported approximately four presents per rendered frame.

The following GPU values use the post-initialization window after the last timing
contract change and first active status, with 59 model samples per run. Its
68.1–68.6 seconds do not exactly equal the game's approximately 64.2-second
benchmark window. The rendered-frame statistics discard the first 5% of that
separate CSV population. Warmups are excluded from this table.

| Run | Outside model GPU p50 / mean (ms) | Model GPU p50 (ms) | Rendered interval mean / p99 (ms) | Reported FPS |
|---|---:|---:|---:|---:|
| old1 | 0.447 / 0.486 | 3.631 | 37.680 / 43.351 | 106.031 |
| new1 | 0.406 / 0.421 | 3.631 | 37.862 / 43.529 | 105.545 |
| new2 | 0.403 / 0.429 | 3.627 | 37.899 / 43.538 | 105.439 |
| old2 | 0.442 / 0.469 | 3.635 | 37.904 / 43.030 | 105.375 |

The approximately 0.04 ms separation is consistent across both new runs, about
9% of the measured auxiliary median. It is not a 9% FPS gain. Pooling the
post-initialization samples yields an outside-model mean of 0.478 -> 0.425 ms
and an NR dispatch mean of 4.173 -> 4.108 ms (118 samples per build).
The dispatch p99 went from 4.636 to 4.853 ms and the model p99 from 4.209 to
4.446 ms; those tails do not support a blanket latency improvement.

All sampled post-initialization frames in this adaptive comparison ran the model.
The wider active log window spent roughly 95% of its time at effective cadence 1;
that proportion includes initialization and is not the benchmark CSV population.
These conditions explain why unused reductions can be removed in this workload.

The average reported FPS was 105.703 old versus 105.492 new; the corresponding
mean rendered intervals were 37.792 versus 37.881 ms. The between-build difference
is smaller than the old1/old2 drift. It establishes neither an FPS gain nor a
causal overall regression or statistical equivalence. Each build has only two
independent game runs; within-run GPU samples are not independent repetitions.

### Conditions and instrumentation limits

Measured GPU temperatures averaged 65.9–66.5 C, peaking at 70–71 C. Mean graphics
clocks were 2714–2727 MHz and mean board power 397–402 W. These recorded conditions
are context, not a correction model.

The original helper's load-average threshold marked runs as contaminated even
though its compiler-lock check stayed clear; the game's own CPU load contributes
to that threshold. Separately, a 5-second vmstat trace confirmed **unequal actual
paging**: new2 reached 203798 KiB/s swap-out, while old2 averaged 24264 KiB/s
swap-in. The first vmstat row (since-boot averages) was excluded. Monitoring
covered only the latter part of old1 and all later measured runs. ABBA does not
cancel such unequal interference; global results and tails remain limited.

The initial mapped-module helper missed processes whose name changed to
`GameThread`. Its warnings are retained. Every run's log has the expected build
stamp and DXGI working mode; deployment hashes were verified at each swap.
A separate live observer correlated mapped proxy/forwarder paths, inodes and disk
hashes in old2 and C2, explicitly recording the Btrfs device-number discrepancy.
It did not retrospectively prove mapped hashes for all earlier new runs and did
not read process memory.

All measured runs reported zero OptiScaler errors and zero NR warnings. Fifteen
Streamline-forwarded error messages per run were already present in both arms;
they are not represented as a new candidate regression.

## First-carry cost: separate fixed-C2 pair

One old/new pair used `Cadence=2`, adaptive off, smoothing on and
`GpuTimingInterval=1`. Every successful carry in this configuration is a first
carry. This experiment intentionally changes cadence and instrumentation density;
its FPS must not be compared with the adaptive ABBA results.

All valid first-carry samples, including initialization:

| Metric | Old, n=1012 | New, n=1015 | Difference |
|---|---:|---:|---:|
| Mean GPU dispatch | 1.159 ms | 1.249 ms | +0.090 ms |
| Median GPU dispatch | 1.085 ms | 1.184 ms | +0.099 ms |
| p99 GPU dispatch | 2.283 ms | 2.353 ms | +0.070 ms |

After initialization, the first-carry mean/p50/p99 was
1.161/1.086/2.289 ms (n=1004) old versus 1.257/1.194/2.360 ms (n=985) new.
Model-frame dispatch means were 5.249 -> 5.173 ms. Taking the arithmetic mean of
**all observed dispatch samples**, including both classes, gave
**3.208 -> 3.218 ms** per sample (2011 old, 1973 new). Outside-model means were
1.382 -> 1.400 ms. This is a small observed increase in the aggregate, not a net
C2 speedup and not a sum of category medians.

The game's rendered interval mean/p99 was 37.42/44.57 ms old versus
37.35/43.89 ms new, with reported FPS 106.7 -> 107.0. Paging was again unequal.
One pair and roughly a thousand first carries characterize this run but do not
establish repeatability of its tail. No later-carry performance comparison was
performed in the game; fixed C4 replay checks its output correctness.

## Acceptance and evidence boundary

The local acceptance is for the current adaptive configuration: repeated model
frames showed a small consistent auxiliary saving, output stayed identical in the
replays, and the explicit C2 check characterized the approximately 0.1 ms transfer
to first carry. The optimization is not qualified as a universal speedup for
frequent-carry scenes, other GPUs or native Windows. It is not evidence that the
project has reached an absolute performance maximum.

The [source review](../../superpowers/reviews/2026-11-01-rtx4090-cadence-review.md)
records the corrected test gaps, ASan/UBSan regression results, shader regeneration
and successful Release build. The initial host tier was 121 PASS plus one missing
NvAPI prerequisite; after populating the exact dependency gitlink, the failed
suite alone passed. All 122 suites have passing evidence; the original invocation
is not renamed a clean PASS.

Raw evidence is outside Git in the local `2026-11-01-4090-perf` cache record,
protected by its existing `.keep` marker:

- `trials/20261101T153718-base-175bc188` and
  `trials/20261101T153718-cand-a445fe1c53d1`: baseline/candidate replay manifests.
- `trials/20261101T1600-aa-base-175bc188`: fresh-process A/A identity control.
- `bench-20261101T153945`: warmups, adaptive ABBA raw logs, CSVs, conditions,
  `results.md` and corrected `abba-analysis.md`.
- `bench-20261101T155427`: dedicated C2 logs, CSVs and `c2-analysis.md`.
- `memory-watch-controller.log`, `mapped-proxy-independent.jsonl`,
  frozen bundle checksums and receipt logs preserve runtime provenance.

The game INI and model DLLs were restored byte-for-byte after experiments.
At 19:01:25 UTC, the receipt installer completed final local activation of the
frozen tested candidate, with the fixed baseline forwarder. The baseline bundle
is retained for rollback. `activation-20261101` contains the complete receipt,
before/after checksums, active-state checksum manifest and rollback instructions.
The game was closed for the swap; it was not relaunched after this final install.
The identical candidate hash had already loaded in the measured new runs.
Both temporary observers have ended. No commit, merge or push was performed.
