# Present-guide extraction implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:executing-plans for the bounded implementer task; the coordinator owns delegation and final review. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Extract D3D12 present-guide responsibility while retaining behavior and state identity.
**Architecture:** One new responsibility unit, minimal internal declarations, existing core storage and public APIs unchanged. Source-slice tests follow explicit owners; no new renderer framework.
**Tech Stack:** C++/MSVC, Python host fixtures, ASan/UBSan, clang-format 22.
**Spec:** [present-guides-extraction](../specs/2026-10-29-present-guides-extraction.md).

## Global constraints

Base `dd2a589108652870a702313678cf67cf8a98ced7`. Isolated worktree only. No C++ logic, INI, shader, model or FG changes. No commit/merge/push/deploy. At most four host workers. Only the coordinator admits build/GPU work. Never stop shared services or a shared Wine prefix. Preserve all original evidence.

## Review focus

- Shared-storage order, initializer and identity across translation units: retain `nr-dx12-internal` contract and negative link controls.
- No-guide/off paths, accepted/refused/consumed guides, lag and route-switch resets: keep actual glue behavior in `nr-present-guide-identity`.
- Device/format/copy flags, barriers and tracking before writing: keep capture/transport contracts and immutable moved-body comparison.
- Test slices crossing removed anchors: use explicit owner files, never make assertions optional or duplicate logic into fakes.
- Host/after-upscale coverage is not live present/D3D11/native-Windows qualification: record each limit and do not promote.

## Task 1: Baseline and extraction (Codex)

**Files:** create `retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp`; modify `DlssNr_Dx12.cpp`, `DlssNr_Dx12_Internal.h`, fork tails of `OptiScaler/OptiScaler.vcxproj` and `.vcxproj.filters`, relevant `tests/nr-dx12-internal/` ownership/body contracts and existing guide/source-region fixtures. Update only directly affected README/design indexes. The coordinator owns this plan and spec.
**Consumes:** the base source and existing shared-state/function-owner contracts.
**Produces:** a tested uncommitted extraction diff, unchanged public signatures and explicit minimal internal helper declarations.

- [x] Read module guides, candidate functions/callers and ownership contracts; exact 27-symbol list is in `tests/nr-dx12-internal/present-guides-contract.json`.
- [x] Coordinator ran the six focused baseline suites natively on the clean base: 6 PASS. The implementer's sandbox failed under ptrace/LeakSanitizer; those runs were retained, not accepted as the baseline.
- [x] Add extraction owner/body contracts before moving functions; observe RED and GREEN; keep earlier state/resource hashes unchanged. Add decision/tracking-order negative controls.
- [x] Extract the chosen functions, keep storage/initializers and temporal release in core, add private declarations and project registration; adapt fixtures without dropping behavioral assertions.
- [x] Focused exploratory tests passed; coordinator ran full native host validation with sanitizers intact, clang-format 22 and docs/diff checks. Details and failures retained in the review record.
- [x] Implementer stopped with an uncommitted diff; no build/Wine/GPU/commit/merge/push or further agent delegation by the implementer.

## Task 2: Independent validation and review (coordinator + Claude)

**Files:** read the full worktree diff; add a dated review record only after actual results.
**Consumes:** Task 1 diff and exact commands/results; immutable baseline.
**Produces:** verified source result and explicit runtime qualification status; no main-branch integration.

- [x] Inspect the full diff, independent original-body hashes, storage/linkage and tests; no renderer-default, shader or unrelated feature changes.
- [x] Claude independently reviewed scope and quality; one corrective pass addressed private namespace, documentation, fixture uniqueness, token boundaries and comments with regressions.
- [x] Re-run full host tier independently after corrections: 100 PASS, zero aggregate non-PASS, source stable; formatting/docs/privacy/diff checks passed.
- [x] Run serialized Release and MSVC identity checks successfully; additionally run the real-device D3D11 transport fixture (8/8). Retain original/candidate binary hashes. These do not qualify candidate live-present/game behavior.
- [x] Record completed gates and missing runtime evidence; leave implementation isolated, uncommitted and uninstalled. See the [review](../reviews/2026-10-29-present-guides-extraction.md).

## Coordination ledger

Plan identity: `2026-10-29-present-guides-extraction`.
Herdr isolated branch: `refactor/present-guides-20261029`.
Ruling: the latest user message authorizes delegating the previously proposed first lot, not committing/merging/pushing a new change.
Ruling: implementer uses a fresh Codex session; the existing Claude session provides project-specific read-only review; only one writer owns code.
Preflight: the reviewer consumes the implementation diff; no parallel edits to its files. Runtime admission stays with the coordinator to avoid conflicting with games or builds.

Task 1: source implementation completed by the dedicated Codex session, followed by one corrective pass after Claude review.
Task 2: coordinator source/build/fixture gates completed; full runtime qualification remains below.
Ruling: keep `ReleasePresentTemporal` and all shutdown orchestration in core — narrows the move and preserves existing release/lock context.
Ruling: LSan under the implementer's ptrace sandbox is not an acceptance environment — no suppression was added to production tests; native final runs removed ASAN_OPTIONS/LSAN_OPTIONS and passed.
Ruling: `present-guides-contract.json` is the dedicated owner/body registry for the 27 functions — existing resource and state contracts remain untouched.
Ruling: preserve both input corpus and before/after binaries outside disposable cache; old frozen output baselines are absent and must not be invented.
Final review: independent Claude review followed by coordinator inspection and regression verification of corrections, not a claimed second independent review.
Deferred minor: the new unit retains the broad original include block; include minimization is not part of this extraction.

## Runtime qualification before promotion

- [x] Recreate and retain original-DLL output baselines; compare 35 before/after variant-mode pairs across 256 frames each. These are new baselines, not the missing historical ones; this is after-upscale coverage only.
- [x] Exercise the DLL-backed bounded D3D12 present-guide case: 34 uses in each binary. This is guide-use coverage, not final-present pixel equivalence.
- [ ] Resolve the DLL-backed native D3D11 bridge run: both runs transported/used guides but **FAIL** during `Shutdown1`; no cleanup bypass or PASS is authorized. See the [runtime review](../reviews/2026-10-29-present-guides-runtime.md).
- [x] Run the final native host tier: 100 PASS, zero FAIL/SKIP/TIMEOUT/ABORTED/INCONCLUSIVE, sanitizers intact; `artifacts/present-guides-runtime-final/summary.json`, run ID `36244314349044d18327bcb8a0142787`, HEAD `dd2a5891`, `dirty=true`. Non-Markdown sources were stable; documentation changed during the run.
- [ ] Repeat final documentation and privacy checks after the documentation changes; the coordinator owns these checks.
- [ ] Obtain coverage for resize, route switching/lag, final present pixels and scoped native Windows/runtime validation. These are not established by the bounded cases above.

Integration remains blocked by the D3D11 shutdown failure and outstanding qualification. No commit, merge, push,
release or game installation was performed in this task.
