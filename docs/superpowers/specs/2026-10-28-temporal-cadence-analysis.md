# Temporal cadence evidence and bounded experiment

## Intent and authorization

The user approved the proposed next batch: localize temporal deviations in retained captures, compare GPU cost and quality by phase, perform one small evidence-led cadence experiment, and validate it on a reserved scenario. This document makes that approved batch concrete. The baseline is `aeac85f3`; historical rendering evidence belongs to qualified source `abad9704` and its recorded binary hashes.

## Approach

Reuse the existing replay judge, capture formats and production cadence options. Add an offline event analysis tool, then test `Cadence=2` with adaptive admission against `Cadence=4` with the same admission. Preserve the numerical fidelity judge and its policy. A new, fixed reverse-trajectory corpus is reserved before new GPU experiments.

Alternatives considered: changing smoothing first does not directly address the observed `every->every` peaks; adding a new reset rule would alter model history without a demonstrated contract violation. The chosen experiment changes only an existing maximum-interval setting. It can be rejected without shipping a renderer change. No default, shader, new configuration key, model binary, game installation, or remote branch publication is required.

## Offline analysis contract

`tools/replay/events.py` exposes `analyze(capture_path, runs_dir, judge_dir, candidate, *, control='every-b', reference='every-a', qualification=None, window=4, top=8)` and a `python -m tools.replay.events` CLI with equivalent arguments plus `--out`.

Inputs are the complete capture and raw run manifests plus the existing candidate/control `result.json` and `frames.jsonl`. Require the same capture/reference identity, environment, policy, metrics revision and regions; a complete every-frame control with calibrated PASS; unique complete frame identities, matching times, presents, decisions and timings. Parse bounded strict JSON/JSONL; reject duplicate keys, nonfinite values and malformed scalar types. Read at most 10,000 frames and 32 MiB per input file, with event window 0..32 and top 1..16. Output must be separate from input paths. No renderer or raw image is executed/rewritten by analysis.

An optional consolidated qualification report verifies retained artifact bytes/hashes for historical inputs. Without that receipt, identify provenance as current run-manifest and input-hash validation, not historically retained evidence. Reconcile scored rows with raw run entries; retain hashes and sizes of every consumed file. Do not reattribute historical captures to the current source HEAD.

Phase metadata comes from the capture's bound `provenance.corpus`, validated for ordered nonoverlapping complete frame coverage and counts; it is not inferred from filenames. Preserve the profile name, reset IDs and phase limits. Reference and control are distinct fresh-process runs.

Derive history from all raw frames, including warmup: model/every execution with successful complete passes renews history, carried increments age, skip invalidates known history, reset starts a new epoch. Keep both current age and the preceding carried streak on a returning model frame. Frame IDs are identities, not arithmetic ages.

For each scored frame and region, retain source/control pulse, signed delta, absolute paired distance and signed amplitude excess: `p_c-p_b`, `abs(p_c-p_b)`, `abs(p_c)-abs(p_b)`. Null on either side stays unavailable, with reason. Decision transition does not imply a valid temporal pair at reset. Preserve spatial metrics and GPU times independently.

Summaries include counts, missing counts, mean, p50/p95/p99, extrema and frame IDs, grouped by phase, execution kind, history age and event. Use the existing interpolated percentile definition. A short group remains descriptive and is labelled as such. Events include phase/interval/reset changes and execution/reuse transitions, plus bounded context around worst paired deviations. Overlapping event labels never duplicate a frame in whole-corpus totals.

Write `events.json`, derived `frames.jsonl` and a readable `events.md` with selected frame IDs/windows. Keep functional validity, existing fidelity verdict and perceptual assessment distinct. GPU time describes the replay harness bracket, not game FPS, display pacing, latency, or isolated kernel busy time.

## Reserved profile

Add `temporal-reverse-v1`, opt-in, alongside unchanged `temporal-v1`. Both have 480 frames at 60 Hz, 80 warmup and the same phase boundaries. Reverse starts at pose 704; slow motion decrements by 2, reaching 544; fast motion decrements by 12, reaching 64; the scored cut goes to pose 32. All other holds retain the actual pose, material follows that pose, jitter follows rendered ordinal, previous pose follows actual history and resets use current=previous. Reset ordinals remain 0 and 360.

The profile must have its own artifact directory, identity and exact validation; it must not be relabelled as temporal-v1. `--replay-profile` is accepted only with `--replay-quality`, defaults to temporal-v1, and is passed through production fixture planning. External capture replay stays on `--replay-nr`. Reject profile/capture/frame-count/legacy-recorder contradictions before runtime. Existing legacy and temporal-v1 behavior remains identical.

This is transfer to a different trajectory of the same synthetic opaque scene. It does not add HDR, exposure-change, independent-object, HUD, dynamic-resolution or game coverage.

## Frozen experiment

Use the audited forward capture as development corpus. Candidate: `Cadence=2`, `CadenceAdaptive=true`, `CadenceSmoothing=false`. Baseline: identical except `Cadence=4`. Controls: NR off, every-a/every-b with cadence 1 and adaptive/smoothing false, and five fresh A/A repeats. Use three fresh-process pairs in AB, BA, AB order, with unique names and trial IDs. Replay is synchronous; cancellation/recreation stress is disabled. Record actual commands, order, identities and failures.

Primary metric: full-frame p95 of paired pulse distance in the fast-motion phase. Experimental continuation requires at least 20% reduction in the median over the three pairs and improvement in each pair. Guard golden MAE, temporal MAE, ghost MAE, disocclusion MAE and detail error for each scored phase in all/centre: no regression over `max(5% of baseline, the matching observed A/A envelope)`. Unavailable temporal metrics at the cut are unavailable, not zero.

GPU budget: scored-frame mean at least 20% below the every-frame control and no more than 30% above adaptive4; report p95/p99 and fast-motion cost. These are predeclared engineering experiment budgets, not calibrated perceptual thresholds. Three pairs produce descriptive paired observations and ranges; do not claim a population confidence interval or treat frames as independent trials.

Require complete successful model passes, no scored skips, interval set {1,2} for adaptive2, actual reuse, and a model execution on each scored reset. Preserve A/A calibrated PASS and every fidelity FAIL. Apply the same frozen criteria to the reserved reverse profile without tuning after seeing its results. A failed or inconclusive candidate is recorded and is not promoted to a default.

## Review and integration

Run focused behavioral tests, the strict host tier, the actual forward/reverse GPU experiment, and an independent source/evidence review. Check the current main checkout before the already authorized local integration. Keep all original evidence and isolated experiment outputs. No push or game deployment is part of this batch.
