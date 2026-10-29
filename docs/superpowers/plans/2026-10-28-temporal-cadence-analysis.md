# Temporal Cadence Analysis Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development for the independent analysis tool and profile tasks; one remote executor owns canonical edits, compiler and GPU operations. Steps use checkboxes.

**Goal:** Deliver reproducible temporal event/cost analysis and a measured, predeclared adaptive2 experiment on development and reserved trajectories.

**Architecture:** Offline analysis consumes the current judge and run manifests. An opt-in reverse profile reuses the existing moving-plan renderer. Runtime experiments use existing cadence controls and the qualified binary pair; rendering behavior is not changed speculatively.

**Tech Stack:** Python stdlib, current optional NumPy replay backend, existing C++ moving-plan harness under Proton, host unittest suites.

**Spec:** `docs/superpowers/specs/2026-10-28-temporal-cadence-analysis.md`

## Global Constraints

- Baseline `aeac85f3`; preserve original qualification worktree/evidence.
- Strict numerical policy remains unchanged; existing FAIL is not perceptual approval.
- 10,000 frames / 32 MiB per input; window 0..32, top 1..16.
- No new renderer/default/shader/model change without measured evidence and required review.
- Compiler/Wine/GPU work serial through the owning executor and supported prefix lock.
- Three independent paired trials, AB/BA/AB; keep source and artifact identities separate.
- No push, game deployment, global process cleanup or modification of another checkout's runtime artifacts.

## Review Focus

- Swapped or edited evidence: hashes, identities, scalar types and frame completeness must fail explicitly.
- A reset with a non-null decision transition: temporal absence must survive aggregation.
- Warmup and malformed frame identities: reject ID gaps under the existing schema; history age comes from evaluations and actual successful model work.
- A missing or tiny phase/region: preserve counts and unavailable metrics; no fabricated tail certainty.
- Unsupported CLI combinations and profile relabelling: reject before compilation and preserve default profile paths.

## Task 1: Offline temporal analysis

**Files:** Create `tools/replay/events.py`, `tests/nr-replay-metrics/test_events.py`; modify that suite's `run.py`/README only as needed. Root owns tools README integration.

**Interfaces:** `analyze(capture_path, runs_dir, judge_dir, candidate, *, control='every-b', reference='every-a', qualification=None, window=4, top=8)`; `write_report(report, output_dir)`; module CLI. Inputs and output fields follow the spec. No profile module is imported by this tool; consume bound capture phase metadata.

- [x] Write behavioral controls for equal/nonzero pulses, opposite signs, reset/missing pairs, warmup age and preceding carry streak, malformed IDs/provenance/finites, phase coverage, quantiles and CLI output isolation.
- [x] Run the focused tests and observe expected missing-feature failures.
- [x] Implement bounded input/identity validation, history, pairing, events, summaries and deterministic outputs.
- [x] Run the complete replay-metrics suite; require PASS with no skipped behavior.
- [x] Independently review source and focused behavior; resolve material defects and record tests.
- [x] Apply and verify on canonical isolated worktree; commit scoped files.

## Task 2: Reserved reverse trajectory

**Files:** Modify `tests/dlssnr-loopback/replay_quality.py`, `run_replay.py`, `run.py`; add focused tests to existing rendering-orchestration suite and document the new profile.

**Interfaces:** Existing no-argument temporal_frames/validation behavior remains temporal-v1. Add explicit profile selection propagated through generation, validation, phase observations and CLI. Default quality directory stays `replay-quality-run`; reverse uses `replay-quality-reverse-run`. Rendering code and moving-plan text format are unchanged.

- [x] Write controls for exact reverse poses/history, default forward identity, metadata rejection, CLI dependencies and directory separation.
- [x] Observe RED; implement profile parameterization without weakening existing guards.
- [x] Run replay plan and rendering-orchestration suites; require PASS.
- [x] Review and commit; freeze the reverse plan/hash before rendering it.

## Task 3: Evidence-led experiment and integration

**Files:** Experiment variants/criteria under `tools/replay/experiments/`; final record in `docs/superpowers/reviews/2026-10-28-temporal-cadence-analysis.md`; generated evidence under ignored `x64/logs` and fixture artifacts.

- [x] Run analysis on retained historical evidence with the original qualification hashes; independently inspect event selection and timing scope.
- [x] Freeze the variant order, acceptance criteria, reverse schedule and source identities before new runtime work.
- [x] Reuse the exact qualified harness after verifying its C++/shader sources and executable hash are unchanged; identify qualified DLL/forwarder/model exactly.
- [ ] Run forward development capture and reverse holdout with the same candidate/control pairs and five A/A repeats; retain commands and failures.
- [ ] Analyze every trial by phase, quality and cost; preserve original fidelity verdicts and make one explicit continuation/rejection decision under the frozen criteria.
- [ ] Create bounded visual sequences around selected temporal events using original pixels and consistent scaling; verify frame labels and identities.
- [ ] Run strict host tier, independent whole-branch/evidence review and privacy/whitespace checks.
- [ ] Commit the measured record, integrate by fast-forward if the authorized target remains compatible, verify clean state and preserve evidence.

## Execution rulings

- User approval covers the proposed batch and local integration workflow already established in this session; routine implementation decisions do not require another approval pause.
- Experimental configuration first is the smallest justified intervention: observed adaptive peaks persist during consecutive model frames, so no new reset/smoothing algorithm is assumed to fix them.
- A rejected candidate still completes the experiment. Its settings are not promoted; useful analysis/profile tooling and the truthful result can be integrated.
- The harness compiler helper still has legacy stall recovery that can stop unrelated Wine processes. This task does not need a C++/shader change: use `--skip-build` with the unchanged qualified executable and explicit source/hash checks, avoiding that compiler path. No global Wine reset or process cleanup is authorized.
- Offline v2 analysis validates the existing metadata/descriptor contracts without reading plane bytes; it does not rerun pixel hashes or the image judge. ID gaps are rejected, not treated as a new supported capture format.
- Before reading any new trial metrics, clarify the frozen primary aggregation as `1 - median(candidate p95) / median(baseline p95)`, plus improvement in every pair. Also report the median of paired relative improvements. Phase quality guards use arithmetic means, as in the existing judge; the matching-region A/A envelope is the observed metric maximum, not a phase-specific estimate.
