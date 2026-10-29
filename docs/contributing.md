# Contributing to Retoque

Start with [architecture](architecture.md) and [capabilities](capabilities.md), then read the README beside
the module you will change. The upstream [PCH rules](../CONTRIBUTING.md) still apply.

## Before editing

Check `git status`, `git worktree list` and recent history. The main checkout may be in use by a human or another
agent. Preserve unrelated changes and use a branch/worktree for work not yet validated. Commit, merge and push
only when authorized; the default branch has historically been mirrored to the public remote.

Keep upstream paths stable. Do not turn a focused bug fix into a whole-tree cleanup. New NR behavior starts with
a design note explaining intent, off-path behavior, resources, reset conditions and acceptance evidence. Record
why a proposal was rejected as carefully as why it was accepted.

## Small, reviewable changes

Separate structural extraction from behavior changes. Keep public entrypoints, shared-state initialization,
lock order and GPU resource lifetime stable during extraction. Existing D3D12 split contracts are indexed in
[architecture](architecture.md#renderer-boundaries). A smaller file is not evidence of unchanged behavior.

For config changes, update declaration/default, parser, persistence and shipped INI together. For shaders,
regenerate applicable DXIL/SPIR-V headers and verify matching constant-buffer layout. Never format generated
bytecode. Use clang-format 22 for C++; see [maintenance](maintenance.md#formatting-and-shaders).

## Tests and evidence

Python 3.11+ is needed for the stdlib TOML registry; host CI uses Python 3.12. Choose suites from the generated
[catalog](../tests/CATALOG.md). Each new suite needs its own README/run.py and an entry in `tests/suites.toml`.

```bash
python3 tests/run_all.py --list --group-by domain
python3 tests/run_all.py --domain nr --strict
python3 tests/run_all.py --tier host --jobs 4 --strict
python3 tools/check_docs.py --write-catalog
```

Write a regression that fails before fixing the defect, then run the affected tests and the complete host tier.
Host tests exercise contracts, not pixels or native GPU behavior. GPU/Wine work is serial under the shared-prefix
rules. Report failed, skipped, inconclusive and not-run cases; do not hide them behind an overall success.

A rendering change also needs the change-type review in [DEVELOPMENT.md](dlssnr/design/DEVELOPMENT.md), build and
appropriate runtime evidence. An after-upscale replay does not automatically cover present, pre-upscale or
D3D11 bridges. New D3D12 passes need Windows-native validation as well as Proton runs.

## Documentation changes

Update the authoritative document, not every historical note. Keep current capability status and dated evidence
separate. The roadmap records priorities/blocks; it is not a second compatibility matrix. Preserve links when
moving documents. `tools/check_docs.py` checks local destinations and generated catalog drift, not external URLs,
heading anchors or the truth of a technical statement.

## Review and release

A review should identify the change's scope, the source commit and dirty state, exact commands/outcomes,
material evidence paths/hashes, known coverage gaps and any decisions made without runtime proof. Never put
personal absolute paths, credentials or raw proprietary model data in public records.

A package is not qualified merely because its source tests pass. Follow [release validation](releases/VALIDATION.md)
and keep publication a separate human decision. User-supplied NR/LSFG models stay outside distributed artifacts.
