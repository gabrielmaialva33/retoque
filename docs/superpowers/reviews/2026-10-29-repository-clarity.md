# Repository clarity and traceability — 2026-10-29 review

Status: implementation reviewed in the isolated `chore/repo-clarity-traceability-20261029` worktree.
The user subsequently authorized commit and local integration into `dlss-neural-rendering` on 2026-10-29,
but not push, publication or deployment. Scope: the approved organizational steps 1 and 2 only.
The measurements below describe the original uncommitted implementation, not a post-merge qualification.

## Source and scope

Base: `9778aeb502648d50e00bc1963329a3b5fa34a9ae`. The main checkout advanced while the isolated worktree was created;
`da751da7` is earlier context, not this worktree's base. Concurrent main-checkout changes were not edited here.

The diff changes documentation, the existing test runner, two host suites, two stdlib tooling commands and two CI
workflows. `OptiScaler/`, `retoque/`, `OptiScaler.ini`, shaders and rendering defaults have no diff against the base.
`AGENTS.md` remains a symlink to the concise current `CLAUDE.md`. Full old guide and roadmap content are preserved
with historical notices; no rejected experiment was removed.

## Implemented

- Current contributor, architecture and maintenance guides; explicit documentation authority and refreshed roadmap.
- Capability index separates implementation, activation/defaults, dated evidence and coverage gaps.
- Existing runner gains domain intersection, grouped listing and deterministic Markdown generation; stable suite IDs
  and host/Wine execution defaults remain. `tests/CATALOG.md` is derived from `suites.toml`.
- Documentation checker validates local Markdown/HTML destinations and catalog drift; it does not fetch URLs or
  validate headings/technical claims.
- Release manifest hashes the archive and members, validates complete clean same-commit source evidence and exports
  only a sanitized summary. It explicitly records runtime `NOT_RUN` and does not claim compiler provenance from hashes.
- CI draft requires same-workflow host evidence, uses the producer's explicit artifact name/run ID and rechecks archive
  identity before creating a draft. Only the draft job has write permission. Publication remains a separate human action.

## Validation performed

Baseline runner: 33 tests PASS before changes. New behavior first failed under focused regressions, then passed:
runner discovery 6 added cases; documentation 6 cases; release evidence 14 cases. The strict-empty-selection
regression was also observed failing before the one-line exit-status fix.

Complete host command:

```bash
python3 tests/run_all.py --tier host --jobs 4 --strict --log-dir artifacts/repo-clarity-host
```

Result: **100 PASS**, zero FAIL/SKIP/TIMEOUT/ABORTED/INCONCLUSIVE in the aggregate.
Invocation: `b90f748581864a5d8a530f63f3f788d7`; finished `2026-10-29T20:02:09.615560+00:00`.
Report: `artifacts/repo-clarity-host/summary.json` in the execution worktree (local, ignored).
The report truthfully says `dirty=true`: these are uncommitted implementation checks, not clean release qualification.
The release validator must refuse to use this report as a release gate without a subsequent clean-source run.

Selected underlying results: runner-results **40 tests PASS**; release-evidence **14 PASS**; repo-docs **6 PASS**;
Windows-installer **31 fixture cases PASS** in its PowerShell container; privacy check passed across 1,832 indexed files.
The installer cases do not execute Windows-native DLLs. Many legacy suites use process-exit evidence; an aggregate
PASS does not expand the scenario coverage declared in each suite README.

Documentation checker passed for 264 Markdown files before this review record was added. `git diff --check` passed.
After the host run, only the workflow's artifact routing was refined for partial retries, then validated separately:

- Both workflow files parsed with the available PyYAML; every Bash run block passed `bash -n`.
- Every PowerShell run block passed the installed PowerShell AST parser; the upload JavaScript passed Node syntax check.
- The actual upload script ran against in-memory filesystem/API boundaries: valid draft, changed archive, wrong commit,
  dirty source, failed host evidence and existing release. All six cases behaved as required; rejected cases made no API
  mutations. This did not contact GitHub or create any real release.
- Static dependency checks confirmed `draft` needs host-checks and downloads the artifact name exported by its producer,
  rather than constructing a new name from the consumer's retry count.

## Review findings and rulings

**Resolved:** strict empty selection returned exit 0 despite the documented strict contract. Preserve INCONCLUSIVE
reporting and non-strict behavior, but return 1 under strict. Existing terminal-signal tests still pass.

**Resolved:** constructing a download name from the draft's current attempt would lose the successful producer's
artifact during a failed-job-only retry. Export its actual name alongside its run ID and consume those outputs.

**Intentional deviation:** leave `package_release.ps1` usable in PowerShell-only/container environments. The Python
manifest is a mandatory post-packaging CI step and an explicit local command, not a new dependency of the packager.
A standalone local zip is not automatically qualified.

**Intentional location:** the historical roadmap stays beside its original `design/` targets to preserve relative
links. A prominent notice makes it non-authoritative.

Review was a separate self-review of the diff, not a claimed independent agent review. No critical or important
unresolved issue was found within the approved source-only scope. Before integration, reconcile any newer
main-branch work and rerun the source checks; the user's subsequent commit/merge authorization is recorded above.

## Declined to certify

- Hosted GitHub Actions execution, hosted runner images/actions availability and artifact retention behavior: no workflow
  was dispatched. Local parsing and simulations are not a hosted run. `actionlint` was not installed; it was not run.
- Native Windows release build, real package generation on Windows, DLL GPU behavior and performance: out of this
  source-only organizational task. No GPU/Wine suite, game installation or renderer build was run.
- External URL reachability, heading anchors and all historical technical claims: the docs checker deliberately
  covers local destinations/catalog drift, not semantic or external verification.
- Cryptographic authenticity/compiler provenance: SHA-256 binds bytes, not authorship or proof of a build's source.
- More structural renderer extraction and in-game diagnostic export: separate future tasks, not silently included.

## Recheck before integration

```bash
python3 tools/check_docs.py
python3 tests/run_all.py --only runner-results,repo-docs,release-evidence,repo-privacy --strict --log-dir artifacts/repo-clarity-final
python3 tests/run_all.py --tier host --jobs 4 --strict
git diff --check
```

The integration was subsequently authorized. Preserve the original local evidence, verify the committed/merged
source separately, and do not discard other agents' files or publish a release as part of this operation.
