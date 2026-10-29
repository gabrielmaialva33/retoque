# Temporal cadence analysis and adaptive2 experiment — 2026-10-28

## Outcome

The predeclared adaptive2 candidate is **rejected on both trajectories**.
On the development trajectory, its fast-motion full-frame paired-distance p95 is
46.27% higher than adaptive4 in each of three paired trials. On the reserved
reverse trajectory, that same primary improves by 23.01%, but ten quality guards
still fail per pair and the first pair exceeds the GPU budget against adaptive4.
No adaptive2 setting is promoted.

| Frozen gate | Development | Reserved reverse |
| --- | --- | --- |
| Primary: at least 20% improvement, every pair improves | FAIL; p95 46.27% higher | PASS; p95 23.01% lower |
| Quality guards | FAIL; 10 combinations per pair | FAIL; 10 combinations per pair |
| Whole-scored GPU budget | PASS / PASS / PASS | FAIL / PASS / PASS |
| Functional execution | PASS | PASS |
| Experiment decision | REJECT | REJECT |

The full strict host tier passes **94/94 suites**, with no failures, skips,
timeouts, aborted or inconclusive suites. Both runtime experiments completed
successfully; their numerical and experimental rejections remain explicit.

The deliverable adds an offline temporal-event analyzer and an opt-in reverse
trajectory to the replay fixture. The production renderer, shaders, model and
shipped defaults are unchanged. The configuration experiment compares the
existing adaptive maximum intervals 2 and 4, using the qualified binary pair.

Numerical fidelity, functional execution and experimental continuation are
separate conclusions. A rejected configuration remains a useful result; it does
not prevent integrating the analysis and reproducibility tools.

## Why this experiment

The historical `temporal-v1` capture and retained judge artifacts were audited
before choosing the intervention. Adaptive4's largest paired pulse deviations
included consecutive model executions during fast motion: frame IDs 355, 356 and
357 were `every -> every`, with interval 1. Frame 356's candidate pulse was
0.0185564673 versus the independent every-frame control's −0.0155261743,
giving paired distance 0.0340826416. This is not simply a carry-to-model transition.

These observations make a shorter earlier reuse history a bounded hypothesis to
test. They do not establish history as the cause, nor show that smoothing or a new
reset rule would repair the deviation. No new renderer algorithm was justified
by this batch.

Historical adaptive4 measurements, for the full-frame `all` region:

| Phase | Model frames / scored | Paired pulse distance p95 | Golden MAE mean | Candidate GPU mean, ms | Every-b GPU mean, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Steady | 20 / 80 | 0.00652494 | 0.000293689 | 0.627400 | 2.252940 |
| Motion | 80 / 80 | 0.00666241 | 0.000267043 | 2.292079 | 2.385801 |
| Recovery | 34 / 80 | 0.00546080 | 0.000275736 | 1.006073 | 2.307004 |
| Fast motion | 39 / 40 | 0.01675167 | 0.000664846 | 2.261631 | 2.191374 |
| Cut | 1 / 1 | unavailable | 0 | 2.185472 | 2.152288 |
| Post-cut recovery | 44 / 119 | 0.00499206 | 0.000182119 | 0.873329 | 2.157286 |

These are historical observations, not the new experiment's trials. The discovery
record is `x64/logs/temporal-discovery.json`, SHA-256
`36b1504324b6153d7ddbb40e91c479ba7bd3c594fac35b6a85a79afd1ac3b27d`.

## Delivered tooling

`tools/replay/events.py` reconciles capture/run manifests with the judge's
`result.json` and `frames.jsonl`. It preserves source identities, policy and
verdicts; validates complete frame coverage, times, decisions and passes; derives
history from the warmup onward; and groups quality/cost by phase, execution kind,
history age and event. It emits `events.json`, derived `frames.jsonl` and a readable
`events.md`, including worst frames and bounded context windows.

Paired pulse distance means `abs(candidate pulse - every-b pulse)` at the same
frame. The report separately retains signed delta and signed amplitude excess.
Equal nonzero pulses have zero distance; a reset or missing measurement is
unavailable rather than zero. Quantiles describe these recorded frames, without
assuming that adjacent frames are independent trials.

The analyzer opens metadata only. Inputs are bounded to 10,000 frames and 32 MiB
per file; output directories must be separate from inputs. With a qualification
receipt, consumed judge artifacts must match their original retained paths,
hashes and sizes. Without a receipt, the report identifies current-manifest/input
hash validation, not historical retention. Neither mode reruns pixel validation.

`temporal-reverse-v1` uses the existing moving-plan renderer and a separate
`replay-quality-reverse-run` directory. It keeps 480 frames, 80 warmup, 60 Hz and
the same phase boundaries. Poses move from 704 to 544 in steps of −2, then to 64
in steps of −12, with a scored cut to 32. Holds keep actual pose/material;
jitter follows the rendered ordinal; previous matrices follow actual history.
The unchanged default is `temporal-v1` in `replay-quality-run`.

The entrypoint accepts `--replay-profile` only with `--replay-quality`, including
the `--replay-profile=value` form. Contradictory capture/frame-count/warmup/recorder
settings fail before staging or acquiring a fixture lease. The selected identity
and declared moving-plan digest are checked through capture production.

For an existing judged capture, run the new analysis without a GPU invocation:

```bash
python3 -m tools.replay.events \
  --capture /local/capture/capture.json \
  --runs /local/replay/runs \
  --judge /local/replay/judge \
  --candidate adaptive2-1 \
  --out /local/analysis/adaptive2-1
```

The default reference/control are every-a/every-b. Add `--qualification` with the
original receipt only when analyzing its retained judge files. Input and output
directories must remain separate. Exit 0 means the metadata analysis completed;
it does not turn the candidate's preserved numerical FAIL into acceptance.

## Predeclared protocol

The committed manifest is `tools/replay/experiments/adaptive2-v1.json`, SHA-256
`5b3c8145591bca0ef5383a28e691d846ce00ec4e959c311e363d9d8ef1d597ae`.
It was committed as `d09663c1f058b0be6de441d81c989705646450a9` before new runtime work.

Both configurations set `CadenceAdaptive=true` and `CadenceSmoothing=false`.
Only the maximum interval differs. The order is off, every-a, every-b,
adaptive4-1, adaptive2-1, adaptive2-2, adaptive4-2, adaptive4-3, adaptive2-3,
then five fresh A/A repeats. Each variant has a fresh process/trial identity.
Replay is synchronous with cancellation/recreation stress disabled.

| Criterion | Frozen decision rule |
| --- | --- |
| Primary | Full-frame fast-motion p95 of aligned paired pulse distance; `1 - median(candidate p95) / median(baseline p95)` at least 20%, and every pair improves |
| Quality guards | Each scored phase, regions `all` and `centre`, mean golden MAE, temporal MAE, ghost MAE, disocclusion MAE and detail error; increase no greater than `max(5% of baseline mean, matching-region observed A/A envelope)` |
| GPU budget | Each pair's candidate mean over all 400 scored frames is at most 80% of every-b and 130% of adaptive4 |
| Functional | Complete successful model passes, no scored skips with NR enabled, actual reuse, adaptive2 scored intervals `{1, 2}`, and model execution on the scored reset |
| Transfer | The same frozen criteria on the reverse trajectory, without retuning |

Before reading any new metrics, the execution ledger clarified the primary as a
ratio of medians, with median paired relative improvement also reported; phase
guards use the existing judge's arithmetic means. The every-frame GPU denominator
is the independent `every-b` control; every-a's cost is also reported. The A/A
allowance is the observed whole-corpus metric maximum for the matching region,
not a phase-specific noise estimate. Policy floors remain zero. These are
engineering experiment budgets, not perceptual thresholds.

The forward capture is the previously audited development corpus. The reserved
reverse plan was fixed before rendering:

| Plan | Bytes | SHA-256 |
| --- | ---: | --- |
| Forward | 20046 | `03f4e5ec2feb4b95bfc416ab2b09d0af71e2743b7a1f8eb9564288bfc064bf97` |
| Reverse | 20396 | `5d6443de0189e2675964de7d7cabf3b6f12fba2d4184d891c81130683a3e8659` |

## New measurements and decision

### Development trajectory: REJECT

The forward invocation ran from 2026-10-29 00:09:48.499 to 00:32:32.533 UTC,
22 min 44.034 s, with preflight and runtime exit 0. Runtime completion is separate
from the experiment's rejection. The frozen assessor exits 1 for this valid
rejection, not for invalid input.

| Pair / execution order | Adaptive4 primary p95 | Adaptive2 primary p95 | Relative improvement | Adaptive4 scored GPU mean, ms | Adaptive2 scored GPU mean, ms | Candidate / every-b | Candidate / baseline |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 / AB | 0.016751668743 | 0.024502806827 | −46.270841% | 1.30593216 | 1.62962096 | 72.9156% | 124.7860% |
| 2 / BA | 0.016751668743 | 0.024502806827 | −46.270841% | 1.34806280 | 1.63710680 | 73.2505% | 121.4414% |
| 3 / AB | 0.016751668743 | 0.024502806827 | −46.270841% | 1.25499624 | 1.60519736 | 71.8228% | 127.9046% |

The primary ratio of medians and the separately reported median paired relative
improvement are both −0.4627084145365028, below the required +0.20. None of the
three pairs improves. Each p95 summarizes 40 aligned fast-motion observations;
these are not 40 independent experimental trials.

The p95 result is specific. Over those same 40 pairs, mean distance decreases
from 0.009002695741 to 0.008566903759 (−4.84%), p50 decreases 21.30%, p99 decreases
8.78%, and the maximum decreases from 0.034082641614 to 0.027898644300 (−18.14%).
It would be incorrect to describe all temporal errors as 46.27% worse or to
choose one of these favorable statistics as a replacement acceptance criterion.

Every-b's mean over the 400 scored frames is 2.23494168 ms; every-a's is
2.40651248 ms. Adaptive2 is 26.75–28.18% below every-b, but 21.44–27.90% above its
paired adaptive4 baseline. All three pairs meet both predeclared GPU budgets.
Those budgets apply to the complete scored corpus: in fast-motion alone,
adaptive2 costs 2.196385–2.298133 ms against every-b's 2.158364 ms. The result
does not establish a fast-motion GPU saving.

The following first-pair phase table keeps the paired temporal measure and cost
on the same phase. Regions are `all`; complete `all`/`centre` statistics, missing
counts and per-trial distributions remain in the event reports.

| Phase | Scored frames | Adaptive4 distance p95 | Adaptive2 distance p95 | Adaptive4 golden MAE mean | Adaptive2 golden MAE mean | Adaptive4 / adaptive2 GPU mean, ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Steady | 80 | 0.006524938 | 0.006229438 | 0.000293689 | 0.000277192 | 0.677968 / 1.180696 |
| Motion | 80 | 0.006662413 | 0.006659560 | 0.000267043 | 0.000274839 | 2.394921 / 2.351104 |
| Recovery | 80 | 0.005460801 | 0.005788476 | 0.000275736 | 0.000278215 | 1.022526 / 1.471791 |
| Fast motion | 40 | 0.016751669 | 0.024502807 | 0.000664846 | 0.000640672 | 2.218319 / 2.250754 |
| Cut | 1 | unavailable | unavailable | 0 | 0 | 2.170784 / 2.215680 |
| Post-cut recovery | 119 | 0.004992063 | 0.006123052 | 0.000182119 | 0.000198191 | 0.872572 / 1.338784 |

There are ten unique failing phase/region/metric guards, repeated with identical
quality values in each of the three pairs. Thirty failed rows do not represent
thirty independent defects. Golden and ghost MAE coincide in the combinations
below and count as two guards.

| Phase / region | Metric | Baseline mean | Candidate mean | Increase |
| --- | --- | ---: | ---: | ---: |
| Motion / centre | golden MAE, ghost MAE | 0.000597918681 | 0.000630481722 | 5.446% |
| Motion / centre | detail error | 0.007601105753 | 0.008720535584 | 14.727% |
| Fast motion / all | disocclusion MAE | 0.001413472104 | 0.002110397499 | 49.306% |
| Post-cut recovery / all | golden MAE, ghost MAE | 0.000182118998 | 0.000198190949 | 8.825% |
| Post-cut recovery / all | detail error | 0.009757559733 | 0.011065051583 | 13.400% |
| Post-cut recovery / centre | golden MAE, ghost MAE | 0.000391727753 | 0.000447733289 | 14.297% |
| Post-cut recovery / centre | detail error | 0.010387531354 | 0.012836280160 | 23.574% |

For these failed guards the observed A/A spatial envelopes are zero, so their
allowance is 5% of the baseline mean. The A/A observations are not entirely zero:
the temporal MAE envelope is 0.000276150946 for `all` and 0.000612624959 for
`centre`; observed pulse p95 is 0.006252285799 and 0.011237789329 respectively.
These are whole-corpus observations, not calibrated phase-specific error bars.

The complete guard matrix has 46 PASS, 10 FAIL and 14 N/A entries per pair,
with zero UNAVAILABLE. Ten N/A entries belong to the unscored warmup; four are
the expected temporal MAE/disocclusion absence at cut in the two regions.
Spatial cut metrics remain available. PASS means within the stated allowance,
not necessarily a lower mean.

Every-b passes the numerical judge; all six adaptive runs retain their numerical
FAIL. Functional execution passes and perceptual quality remains NOT_ASSESSED.
The two worst full-frame anchors for the first pair are IDs 356 (adaptive4) and
355 (adaptive2), both consecutive-model transitions with interval 1. This again
does not identify the earlier history as the causal mechanism.

### Reserved reverse trajectory: REJECT

The reverse invocation ran from 2026-10-29 00:42:47.351 to 01:05:25.726 UTC,
22 min 38.375 s, with preflight and runtime exit 0. It began and ended at clean
source `09bd7402106298d541ce862d31e922b759fb7243`. The actual recorded 480-sample
moving plan passes the profile validators and matches the predeclared digest.
The frozen assessor returns a valid REJECT, exit 1.

| Pair / execution order | Adaptive4 primary p95 | Adaptive2 primary p95 | Relative improvement | Adaptive4 scored GPU mean, ms | Adaptive2 scored GPU mean, ms | Candidate / every-b | Candidate / baseline | GPU budget |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1 / AB | 0.016241869263 | 0.012505321084 | 23.005654% | 1.27322696 | 1.78886896 | 78.8622% | 140.4988% | FAIL |
| 2 / BA | 0.016241869263 | 0.012505321084 | 23.005654% | 1.35529960 | 1.60964320 | 70.9611% | 118.7666% | PASS |
| 3 / AB | 0.016241869263 | 0.012505321084 | 23.005654% | 1.31844792 | 1.63344280 | 72.0103% | 123.8913% | PASS |

The ratio-of-medians improvement and median paired improvement both equal
0.2300565359071869. All three pairs improve, so the primary passes. Over the same
40 aligned fast-motion observations, mean distance falls 10.21%, p50 falls 5.94%,
p99 falls 18.52%, and the maximum falls 13.07%. These descriptive measures support
the recorded direction within this trajectory; they do not replace the other
frozen gates or establish general transfer to different content.

Every-b's scored GPU mean is 2.26834736 ms and every-a's is 2.19654296 ms. All
three adaptive2 trials meet the 80%-of-every-b budget. Against their paired
adaptive4 baselines, however, their cost increases are 40.50%, 18.77% and 23.89%.
The first pair's 130% ceiling is 1.655195048 ms; its 1.78886896 ms candidate exceeds
that ceiling by 0.133673912 ms. The criterion is per pair: replacing it with a
median or dropping that observation would change the protocol. In fast-motion
alone, adaptive2 costs 2.261883–2.386744 ms versus every-b's 2.163309 ms, so the
whole-corpus saving is not a fast-motion saving.

First-pair reverse phase measurements, region `all`:

| Phase | Scored frames | Adaptive4 distance p95 | Adaptive2 distance p95 | Adaptive4 golden MAE mean | Adaptive2 golden MAE mean | Adaptive4 / adaptive2 GPU mean, ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Steady | 80 | 0.006152929 | 0.005747476 | 0.000417072 | 0.000436920 | 0.626876 / 1.267015 |
| Motion | 80 | 0.007399020 | 0.008287195 | 0.000467541 | 0.000477398 | 2.348769 / 2.475142 |
| Recovery | 80 | 0.005748453 | 0.006661214 | 0.000551972 | 0.000483420 | 1.008741 / 1.518486 |
| Fast motion | 40 | 0.016241869 | 0.012505321 | 0.000540203 | 0.000479897 | 2.150199 / 2.386744 |
| Cut | 1 | unavailable | unavailable | 0 | 0 | 2.191808 / 2.352064 |
| Post-cut recovery | 119 | 0.004992063 | 0.006123052 | 0.000182119 | 0.000198191 | 0.860001 / 1.654406 |

The reverse quality matrix again contains 46 PASS, 10 FAIL and 14 N/A per pair,
zero UNAVAILABLE. Its ten failing combinations have identical quality values
across the three trials:

| Phase / region | Metric | Baseline mean | Candidate mean | Increase |
| --- | --- | ---: | ---: | ---: |
| Steady / centre | golden MAE, ghost MAE | 0.000943633674 | 0.001013083265 | 7.360% |
| Steady / centre | detail error | 0.003318152864 | 0.003729521477 | 12.398% |
| Motion / all | detail error | 0.004126119278 | 0.004359223270 | 5.649% |
| Post-cut recovery / all | golden MAE, ghost MAE | 0.000182118998 | 0.000198190949 | 8.825% |
| Post-cut recovery / all | detail error | 0.009757559733 | 0.011065051583 | 13.400% |
| Post-cut recovery / centre | golden MAE, ghost MAE | 0.000391727753 | 0.000447733289 | 14.297% |
| Post-cut recovery / centre | detail error | 0.010387531354 | 0.012836280160 | 23.574% |

All ten fast-motion guards pass in reverse. Its four failing combinations before
the cut differ from forward's. The other six failures occur in the **shared
post-cut suffix**: both profiles reset to pose 32 and hold the same scene state.
Their quality statistics are identical. They are not six additional independent
holdout failures in different content. Across the two profiles, the union is
14 distinct failing phase/region/metric combinations, with six in common.

The reverse spatial A/A envelopes are zero; its temporal MAE envelopes are
0.000309830723 (`all`) and 0.000707893003 (`centre`). Observed pulse p95 is
0.005957164688 and 0.010379763926 respectively. Guards correctly use the reverse
calibration, retaining the same whole-corpus allowance rule and expected
warmup/cut absences. Numerical fidelity remains FAIL for all six adaptive runs;
every-b passes. Perceptual quality remains NOT_ASSESSED.

### Functional execution and combined decision

Both trajectories show the same scored execution counts in each trial:

| Configuration | Model executions | Carried frames | Skips | Observed scored intervals |
| --- | ---: | ---: | ---: | --- |
| Adaptive4 | 218 | 182 | 0 | `{1, 4}` |
| Adaptive2 | 279 | 121 | 0 | `{1, 2}` |
| Every-frame controls and A/A repeats | 400 | 0 | 0 | `{1}` |

The disabled control has 400 expected skips. NR-enabled trials have complete
successful model passes; scored reset frame ID 361 executes the model. In fast
motion adaptive4 runs the model on 39 of 40 frames and adaptive2 on all 40.
Shortening the maximum interval increases model work over the full sequence by
61 executions, but does not produce a uniformly better quality result.

The frozen candidate is rejected. The opposite primary directions across the
two trajectories rule out claiming a general improvement from this setting.
They do not prove the underlying cause of the deviations. The measured outcome
supports integrating the diagnostic tools and retaining the current defaults;
any history/reset intervention requires its own bounded hypothesis and criteria.

## Selected event sequences

Four contact sheets preserve five actual frame ordinals around each selected
anchor, with columns for input, every-a reference, adaptive4-1 and adaptive2-1.
The originals are 2560 × 2114 PNGs, using one fixed Reinhard/sRGB display mapping,
without per-panel normalization or interpolation. Their metadata retains frame
IDs, times, phases, execution kinds, source identities and selected plane hashes.
Consumed input and plane hashes were checked before and after generation.

| Profile | Selected anchor | Selected by | Included frame IDs | Original PNG SHA-256 |
| --- | ---: | --- | --- | --- |
| Forward | 355 | adaptive2-1 | 353–357 | `4ad8dce93486b3792099dadab79ccaa47eb1516d55cf6066209ebb365baee108` |
| Forward | 356 | adaptive4-1 | 354–358 | `d27e7c034b4c32d664dc9e06b2087af85fe7d2ed24730b8b398b24bffa04d5ae` |
| Reverse | 355 | adaptive2-1 | 353–357 | `73cb22f52d50290ebeae03fc6286a505e45e6acb729e900b9cb3b04441ea8ee3` |
| Reverse | 357 | adaptive4-1 | 355–359 | `d50627d184679de44f90363f3c6caa5878ad4317562304a40f45139f6660f361` |

All anchors lie in fast motion. The four exact derived 1280 × 1057 JPEG previews
were inspected individually for complete rows, column labels, frame/phase/kind
labels and the NOT_ASSESSED footer; no clipped layout was observed. These reduced
previews verify presentation only. Original PNG metadata and source-pixel receipts
remain authoritative; static sequences are not a temporal perceptual assessment.

## Verification and independent review

The event analyzer began with failing behavioral controls. An independent review
found one material retention issue: matching hashes alone ignored the receipt's
retained path. A new control reproduced the problem for nonexistent, relocated,
source-substituted and relative paths. The fix requires the absolute retained
path to resolve to the consumed file as well as matching its bytes/hash. Final
local replay-metrics verification was 47/47 PASS with no skips, independently
repeated. A real v2 raw-DXGI judge/calibration roundtrip also passed after its pixel
files were removed, confirming the metadata-only scope.

The reverse-profile tests began RED and passed after implementation. Independent
review checked all 480 poses/history states against a separate formula, preserved
the exact forward bytes, and exercised CLI/lease/directory propagation. Local
checks: 13 new profile tests, 16 existing quality tests and 7 moving-plan tests
passed. Two native cleanup controls initially skipped in the partial local mirror
subsequently passed on the canonical native host.

Canonical focused verification ran both complete suites: **47 replay-metrics and
120 rendering-orchestration tests PASS, no skips**. The aggregate is
`x64/logs/temporal-focused.json`, run ID `d7bd44e09c5445ffb3e8061053cbadce`, SHA-256
`9fcd08532884bf3f6b0ba7224b1e200d0a3c93e0fbb174523b61af6e2efe4bfa`.
The source was then committed as `3c3ccba5` (events) and `969583b0` (reverse profile).

The independent frozen assessor was tested before consuming new experiment
metrics. Review reproduced and fixed five input/acceptance gaps: nullable ghost
MAE at cut, zero GPU denominators, unbound fixed-profile plan hashes, aliased
output files and incompletely pinned policy values. Its final 21 tests pass with
no skips, independently repeated. This helper is evidence tooling; it does not
alter the checked-in judge or frozen thresholds.

The events analyzer processed three historical adaptive/cadence runs against the
original retention receipt and all six new adaptive trials per trajectory against
their current manifests. All preserve the original numerical FAIL verdicts.
For each trajectory, the export crosscheck compared 432 mean/primary/cost slots
(including 84 explicit nulls) and 864 available/missing counts between the event
reports and the frozen assessor: all agree. Existing forward event files were
revalidated without replacement. A separately reviewed post-runtime exporter
organizes these results; it does not change the frozen assessment or thresholds.

The whole-source review found no remaining blockers. It checked all nine changed
implementation/test/README files, test discovery, default forward behavior,
metadata-only operation and output isolation. This approval covers the tooling,
not adaptive2's acceptance.

The canonical full strict host run selected **94 suites: 94 PASS**, with all other
status counts zero, at clean source `09bd7402106298d541ce862d31e922b759fb7243`.
It includes replay-metrics, rendering-orchestration and repo-privacy. Its run ID
is `ffdb0e5a7fb5403191c3072661b6a859`, report SHA-256
`973e7bdc718d094da9b4f016b6194f077c8d19bea0399cbe9d24e9817a7e21cc`.
It ran from 01:05:46.910 to 01:08:24.065 UTC on 2026-10-29, 2 min 37.155 s.
Only the required simpleini, spdlog and Vulkan submodules were initialized at
their pinned revisions from existing local clones.

Forward evidence review independently recomputed the primary and GPU ratios,
then all 168 available quality-guard calculations and classifications. The
summary matches the complete assessment. Fourteen trial identities are distinct,
and the runtime log records exactly the frozen variant order followed by the five
A/A repeats. The assessment's 31 consumed input receipts preserve capture/run/
judge/calibration identities.

Reverse evidence review independently confirmed its primary, GPU rejection,
quality guards, functional counts and bound capture/plan identities. The
guard review recomputed 168 available allowance/increase/status calculations per
trajectory; 210 region/metric envelope bindings and 1,260 mean/count/missing
fields per trajectory agree with the evidence exports. Null values remain null.
There are 28 distinct trial IDs across both profiles; each runtime log follows
the frozen order. This review explicitly distinguishes the shared post-cut
suffix from the changed trajectory before the cut.

Independent final review found no material blockers or contradictions in the
complete measured report. It confirmed all tables and interpretations, all 94
distinct terminal PASS entries in the exact full-host JSON, and the four preview
hashes/sizes/dimensions. It preserves the distinction between presentation checks
and perceptual assessment, and between a prepared integration and its later
receipt. The reviewed report snapshot was SHA-256
`3eb3c7184b98fd80c639562638df5fc3c8b3d432a6723bfc5b97bdffbc88bf22`;
the final review is `x64/logs/temporal-final-report-review.md`, SHA-256
`eeab7fa764ac14300f992ab2f44883447ccceb498fb637ceb3f1bdb03c7a74e2`.
The only subsequent report edit replaces the pending-review marker with this
approval record.

The successful focused command was:

```bash
python3 tests/run_all.py --only nr-replay-metrics,rendering-orchestration \
  --strict --jobs 2 --report x64/logs/temporal-focused.json \
  --log-dir x64/logs/temporal-focused
python3 tests/run_all.py --tier host --strict --jobs 4 \
  --report x64/logs/temporal-host.json --log-dir x64/logs/temporal-host
```

Runtime commands use the verified existing DLL and harness, the manifest's
environment/variant JSON and five A/A repeats. Forward additionally supplies the
audited capture through `LOOPBACK_REPLAY_CAPTURE`; reverse clears that variable
and records its own corpus:

```bash
python3 tests/dlssnr-loopback/run.py --dll x64/out/OptiScaler.dll \
  --runtime proton --replay-nr --skip-build
python3 tests/dlssnr-loopback/run.py --dll x64/out/OptiScaler.dll \
  --runtime proton --replay-quality \
  --replay-profile temporal-reverse-v1 --skip-build
```

These abbreviated commands are not sufficient without the frozen environment
and verified staged binaries. The exact argument vectors, environment overrides,
source commit, input hashes and start/end times are retained in the two invocation
JSON files. Preflight tests X11/XWayland connection authorization only; actual
GPU/runtime coverage comes from the completed replay and its functional gates.

## Runtime and provenance

The production binaries are the exact previously qualified outputs from
`abad97040570a2849a50ca2b80e49b78c2583e12`. C++/shader sources were verified unchanged;
the existing compiled harness was reused with `--skip-build`. This avoids entering
the fixture's legacy compiler-stall recovery, which can reset a shared Wine
prefix. No compiler reset or unrelated process cleanup was performed.

| Input | SHA-256 |
| --- | --- |
| OptiScaler DLL | `4d48c575e880049ca56dcc6a75d27d88be842621a10a5c66c0e6e820e31a805c` |
| Forwarder | `5bd7d21ff0e9c814d41c9ad6e85ffaa144be53e18573e08e922ee9bc5924d1c8` |
| Harness | `36f2f2152c787d1bf2d93955d8b3770173c254b0d13d2226b643d36019b3535c` |
| Model | `6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927` |
| Audited forward capture | `f17fea2bac406cb0821d98225f720920240d48e45e0a30b40fe9185ed1497c57` |
| Reserved reverse capture | `42e7b2549b34d08b4b61e057eb244108143f02d08f4bc2c3041b64d7542b0dfb` |

The machine reports NVIDIA GeForce RTX 4090, driver 615.78.08, and runtime
`Proton - Experimental`. The forward runtime source is
`d09663c1f058b0be6de441d81c989705646450a9` with a clean worktree; raw run producer
IDs additionally contain the empty-diff hash `e3b0c44298fc1c14`. The historical
capture retains its original producer. The reverse invocation begins at clean
source `09bd7402106298d541ce862d31e922b759fb7243`. These Python/orchestration
source identities are not claims that the production DLL was rebuilt.

| Forward evidence | SHA-256 |
| --- | --- |
| Complete frozen assessment | `b7d8c4a2623b00b066ff58017a6e48ef245c6fda17f25e3d7f56b2bd2e0e5cfb` |
| Measurement summary | `05e81842a4d62025c1a9dfb52d3982e31d852a5035054cbe940339aed1b86131` |
| Historical/current event analysis and crosscheck | `3e73c970a2712df8ff3165cb126b178786c8653ffeff6d51888d120ed99b99c0` |
| Complete independent source review | `1ea708e9651f9210cc50d0a31e612297703f7358d285bfa781662a74f27ede2e` |

Additional retained evidence, under `x64/logs/` in the experiment worktree:

| Artifact | SHA-256 |
| --- | --- |
| `temporal-reverse-assessment.json` | `3cdce33935ff8f2c37c63187b94b5fbfb9649dfcef383d9779e9cd2d23f5a5c3` |
| `temporal-reverse-summary.json` | `365cf24d77c89c676308b0c8b73c2e6ea3da2960fc9549fd2de911b91db71762` |
| `temporal-forward-evidence.json` | `e62b18980ae10cb6cdd579bd4537ff183de83d58cd657644f26e7ca7454b0645` |
| `temporal-reverse-evidence.json` | `77f4bd760fa024a6d38789bfcfbfcdd812c135c7c77d48d6c8a49761b490e82b` |
| `temporal-runtime-analysis-audit.json` | `fd483190276ba706daa878bde439b0e01446b56dfdf6274945124c2243b404c6` |
| `temporal-closeout-evidence.json` | `0bf3803b3f21173c4d77fb8cf4fa604186005197adb54cfdece2fab4678f76ce` |
| `temporal-reverse-evidence-review.md` | `c06c08fbe5b529ffc4fa4ee8902b2074f73e435aa747a0e1737f278ea0f706c2` |
| `temporal-visuals/forward/visuals.json` | `e93a722181b63891488d1c8468935a4aed65787eeb81de1c89499cfbdf225616` |
| `temporal-visuals/reverse/visuals.json` | `67ba322cf75e7de586b8d5b01a9bf6be7f636d4b79d5b541986b6eb13b7b0dba` |

The two evidence exports bind their invocation JSON, runtime log, experiment,
assessment, capture, judge, calibration and event-report receipts. They retain
all trials' actual settings, phase/region statistics, counts and guards. The
runtime audit also checks the recorded reverse moving plan and unchanged binary
identities. The closeout evidence binds the full host report and all four original
PNG / inspection-preview receipts. These artifacts retain local machine paths
as execution evidence; this checked-in record uses repository-relative paths.

Derived event files are under
`x64/logs/temporal-analysis/{historical,forward,reverse}/` in per-variant
directories. Current raw trial/judge files remain under the two separate fixture
artifact directories; historical qualification evidence remains in its original
worktree. Visual originals and their selected-source receipts remain under
`x64/logs/temporal-visuals/{forward,reverse}/`.

The frozen independent assessor has SHA-256
`1f62c5aaca7db8fb36d71f97ad8baca187b51c880db53aff484285a6f0a23dd4`;
the frozen visualization helper has SHA-256
`f2debdc4f6e591e22116d21250ccee6ff1af1a1a05e5b6154da981bf0253ed58`.
The post-runtime evidence exporter has SHA-256
`1f297429d88b90ad31800b09f03ae630c8b03aa715044e73c97aec6c5092efe1`.
The latter was independently tested with five positive/negative behavioral
controls and is an audit/export step, not a predeclared decision-maker.

## Integration record

Implementation commits are `3c3ccba5` for temporal events and `969583b0` for the
reverse profile, after the experiment freeze at `d09663c1`. Source and full-host
verification are anchored to `09bd7402`; subsequent closure changes are confined
to this measured record and the implementation plan. The authorized integration
target is `dlss-neural-rendering`, based on `aeac85f3`. Exact before/after commit
identities, clean-state checks and the local fast-forward result are retained in
`x64/logs/temporal-integration.json` after integration, avoiding a self-referential
commit hash in this report.

## Limits

The test uses another trajectory of the same synthetic opaque scene at fixed
resolution/exposure, with an identical post-cut suffix. It does not qualify HDR,
exposure transitions, HUD, dynamic
resolution, independent object motion, games or native Windows. GPU time is the
replay harness bracket, not FPS, end-to-end latency, display pacing or an isolated
model-kernel measurement. Three paired observations support descriptive results
and ranges only, not a population confidence interval.

Visual sequences retain original source-pixel hashes and a fixed display mapping.
They help inspect selected events; they do not convert numerical FAIL into
perceptual approval. The prior qualified evidence and both experiment worktrees
are retained. No game installation or `git push` is part of this batch.
