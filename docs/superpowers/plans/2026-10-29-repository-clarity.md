# Repository clarity and traceability implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clarify repository authority and connect capabilities, tests and release identity without renderer changes.

**Architecture:** Extend the existing stdlib Python runner and add a post-packaging evidence step to the release workflow. Keep the standalone PowerShell packager dependency-free, upstream paths/test IDs stable and publication manual.

**Tech Stack:** Markdown, Python 3.11+ standard library, PowerShell, GitHub Actions YAML.

**Spec:** [repository-clarity](../specs/2026-10-29-repository-clarity.md).

## Global constraints

No renderer/default changes. No new package dependencies. No commits, merges, pushes, game installs or GPU/Wine workload. Preserve the main checkout and other agents. Never claim host tests qualify pixels, GPU behavior or a specific binary. Keep experimental/rejected evidence.

## Review focus

- List-mode compatibility and combined `--only`/domain/tier/skip selectors; unknown domains must fail (task 1).
- Stale generated tables, missing relative/HTML targets and ignored local artifacts (task 2).
- A PASS with incomplete/duplicate results, dirty/wrong source or fabricated counts (task 3).
- Archive path traversal, case collisions, symlinks and accidental model distribution (task 3).
- `-SkipBuild` provenance and CI dependency cannot turn source checks into runtime approval (tasks 3/4).

## Task 1: Test discovery

Files: `tests/run_all.py`, `tests/runner-results/run.py`, `tests/README.md`.
Interfaces: `selected(manifest,args)` retains current callers; deterministic registry Markdown is exposed for the docs checker.
- [x] Add selection/list regression tests; observe failures.
- [x] Implement domain intersection, grouped listing and Markdown output.
- [x] Run all runner-results tests and exercise the real registry.

## Task 2: Documentation authority

Files: `CLAUDE.md`, `CONTRIBUTING.md`, `docs/README.md`, `docs/contributing.md`, `docs/architecture.md`, `docs/capabilities.md`, `docs/maintenance.md`, `docs/dlssnr/roadmap.md`, dated historical guide, `tools/check_docs.py`, `tests/repo-docs/`, `tests/suites.toml`, `tests/CATALOG.md`.
Interfaces: checker uses task 1 deterministic output; catalog is generated, narrative coverage remains in suite READMEs.
- [x] Test local destinations and catalog drift before implementing checker.
- [x] Preserve old guide; write concise current guides and evidence-aware roadmap/capabilities.
- [x] Generate catalog; run checker and documentation regression suite.

## Task 3: Release identity and source evidence

Files: `tools/release_manifest.py`, `tests/release-evidence/`, `tests/suites.toml`, release workflow, `docs/releases/VALIDATION.md`. The standalone PowerShell packager is intentionally unchanged (ruling below).
Interfaces: JSON sidecar schema `retoque-release-manifest` version 1; host report schema already emitted by `tests/run_all.py`.
- [x] Write adversarial report/zip tests and observe RED.
- [x] Implement streaming hashes, safe archive inventory and complete same-commit source-test validation.
- [x] Add sidecar generation as a mandatory post-packaging CI step; make missing runtime validation explicit.
- [x] Run focused tests, including malformed/partial evidence and package changes.

## Task 4: CI and final validation

Files: `.github/workflows/nr-tests.yml`, `.github/workflows/retoque-release.yml`, guides and review record.
Interfaces: host job uploads this workflow's report; draft job needs it and requires manifest validation before upload.
- [x] Add docs check and matching path triggers.
- [x] Gate release draft on host job, attach zip plus manifest; retain manual publication.
- [x] Run complete host tier, focused checks, diff/privacy checks, YAML/script validation and separate self-review.
- [x] Record results and limitations; leave changes isolated and uncommitted for review.

## Execution ledger

Base: `9778aeb5` (main advanced while the isolated worktree was created).
Baseline: existing `runner-results` 33 tests PASS.
Ruling: execute inline; no subagent tool is provided. Use a final separate self-review.
Ruling: preserve work uncommitted in an isolated branch; committing/publishing was not authorized.
Pre-flight: task 2 consumes task 1 Markdown rendering; task 3 consumes the unchanged runner report schema; task 4 consumes task 3 CLI and existing registry. No interface conflicts found.
Task 1: complete — domain/list regressions observed RED; full runner-results 39/39 PASS, real registry grouping checked.
Task 2: complete — documentation fixtures observed RED; 6/6 PASS; local destinations and catalog checked across 264 Markdown files.
Task 3: complete — release fixtures observed RED; 14/14 PASS, including unsafe archives and false/partial PASS evidence.
Ruling: keep the PowerShell-only packager unchanged and require the Python manifest immediately after packaging in CI — existing installer/container fixtures and local users need a Python-free packager — local standalone ZIPs remain unqualified until the explicit manifest step.
Ruling: retain the historical roadmap beside its original relative targets, rather than under history/ — preserves all historical links — it must stay clearly labeled historical.
Ruling: enforce the runner's documented strict-empty-selection contract — a focused regression reproduced exit 0 with no suites under --strict — keep INCONCLUSIVE evidence but return nonzero, preserving non-strict behavior and signal handling.
Task 4: complete — 100/100 host suites PASS, aggregate skips 0; runner-results 40 tests, Windows installer 31 fixture cases. Both workflows and embedded scripts parsed; six upload-step simulations passed. Hosted Actions/native Windows build/GPU work not run.
Ruling: export the actual source-report artifact name from its producer — a failed-job-only retry must not invent a download name from the consumer attempt — keep report run identity bound as well.
Review record: [2026-10-29-repository-clarity](../reviews/2026-10-29-repository-clarity.md). Separate self-review, not an independent agent verdict. Changes remain uncommitted in the isolated worktree.

## Authorized integration follow-up — 2026-10-29

After the implementation handoff above, the user explicitly approved committing this work and integrating it
locally into `dlss-neural-rendering`. This supersedes only the earlier no-commit/no-merge delivery constraint;
no push, publication, game deployment or renderer changes were authorized. Recheck both worktrees, validate
before committing and run the full host tier on the clean merged source. Keep the existing evidence intact.
