# Build and test hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent compiler-prefix interference and make test summaries distinguish executed coverage from skipped or interrupted runs.

**Architecture:** Keep the public shell build entry, delegate lifecycle supervision to a fork-owned Python helper, and retain standalone test suites. Extend the existing runner with an opt-in structured result contract and atomic aggregate reports; use per-run identity rather than log-text inference.

**Tech Stack:** Python 3.11 standard library, POSIX process groups and flock, existing Bash/MSVC-Wine entry points.

**Spec:** `docs/superpowers/specs/2026-10-27-rendering-hardening.md`

## Global Constraints

- Preserve the primary checkout; no merge, push, game deployment or workstation display changes.
- Do not download or redistribute model/provider assets.
- Stop only processes owned by the operation. A process name is not proof of ownership.
- Use the existing shared compiler-prefix lock. Supported build entry points must acquire it themselves.
- Test results identify executed coverage, not merely successful process exit.

## Review Focus

- An external caller already holds the compiler lock: report busy without hanging or altering build stamps.
- A watchdog timeout occurs while an unrelated process has the same executable name: leave that process alive.
- A prior PASS result exists but this invocation emits none: reject stale evidence.
- A child emits SKIP or INCONCLUSIVE with exit zero: retain that status in the aggregate.
- SIGINT/SIGTERM or timeout arrives while child work is running: terminate only owned work and persist a terminal result.

## Task 1: Own the build lifecycle

**Files:** `tools/build-local.sh`, new `tools/build_local.py`, new `tests/build-lifecycle/run.py`, its README, `tests/suites.toml`, `tools/README.md`, `CLAUDE.md`.

**Interfaces:** shell CLI remains `build-local.sh [Configuration] [MSBuild arguments...]`; Python exposes a bounded command supervisor usable by the behavioral suite. Lock is `$XDG_RUNTIME_DIR/msvc-prefix.lock`, falling back to `/run/user/<uid>/msvc-prefix.lock`.

- [x] Add subprocess-level regressions: held lock refuses before compiler/stamp work; successful fake compiler receives both project invocations and outputs stage; failing compiler preserves previous output; watchdog/interrupt cleanup leaves unrelated matching processes alive; lock is released after exit.
- [x] Run regressions against the baseline and record the expected failures.
- [x] Implement internal nonblocking lock acquisition and process ownership. Preserve output paths, build stamps, configuration/arguments and finite hang detection. Do not infer process ownership from a global name query.
- [x] Preserve failed-attempt evidence and atomically stage outputs only after both projects succeed.
- [x] Run the focused suite, shell/Python syntax checks and an independent review before a real Wine build.
- [x] Update documented invocation and recovery boundaries, commit the tooling batch on the isolated branch.

## Task 2: Propagate structured suite outcomes

**Files:** `tests/run_all.py`, new `tests/_suite_result.py`, new `tests/runner-results/run.py`, its README, `tests/suites.toml`, `tests/fg-inputs-d3d12/run.py`, `tests/fg-provider-d3d12/run.py`, `tests/README.md`.

**Interfaces:** manifest optional `result = "artifacts/result.json"`; `write_result(path, payload)` emits schema `retoque-suite-result`, version 1 and `run_id` from `RETOQUE_TEST_RUN_ID`. Atomic writes replace the target file. Runner aggregate schema is `retoque-test-run`, version 1. `--report PATH` chooses the report; default is the log directory's `summary.json`. `--strict` rejects skipped/inconclusive required suites.

- [x] Add real child-process cases: PASS, SKIP, FAIL, INCONCLUSIVE, missing/malformed/stale/RUNNING result, nonzero exit with claimed PASS, timeout and interruption, plus unchanged exit-only suite behavior.
- [x] Run against the old runner and record the false-PASS reproduction.
- [x] Implement result path validation, per-invocation IDs, status propagation, owned-process cleanup and terminal aggregate persistence.
- [x] Migrate the two FG fixtures through the atomic helper, including RUNNING/FAIL paths, and opt them in through the manifest.
- [x] Exercise the complete runner end-to-end on controlled suites, including signals and strict mode; run the host tier.
- [x] Update documentation and commit the task.

## Task 3: Integration verification and handoff to GPU work

- [x] Review the complete diff independently, addressing important findings with regression tests.
- [ ] Run all host suites with an aggregate report and inspect skipped/inconclusive counts.
- [x] If the compiler prefix and GPU are available, run one Release build through the new owned entry and the D3D12 input fixture serially.
- [x] Record exact results and remaining hardware limits in the progress ledger.
- [x] Continue with the separate V-06/V-02 design and tests; do not count these tooling changes as GPU lifetime fixes.

## Verification record

Task 1 and Task 2 share the registry and documentation and are committed as one reviewed tooling batch. Baseline lock contention returned 124 instead of 75, and an exit-zero structured SKIP was incorrectly PASS; both regressions were observed before implementation. Review findings received behavioral negative controls before their fixes.

Native Arch: build-lifecycle 26/26; runner-results 33/33; provider evidence judge and cleanup controls pass. A real Release build completed through the new entry, then the D3D12 input fixture passed four DirectXMath cases, 64 GPU readbacks and three expected-failure controls. Its debug layer was unavailable.

Full host integration selected 88 suites: 87 PASS, one FAIL in nr-replay-format because importlib loading the runner did not find its new sibling helper. The runner now establishes its sibling import path; the existing nine-case replay-format suite and all 33 runner cases pass. Repeat the complete host aggregate after the Vulkan integration; the earlier aggregate remains an honest FAIL record.
