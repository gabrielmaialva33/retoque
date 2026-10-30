# Present-guide extraction — 2026-10-29 review

Status: source extraction, native host checks, Release build and bounded fixtures completed in the isolated
`refactor/present-guides-20261029` worktree. Uncommitted, not merged, pushed, published or installed in a game.
Full candidate runtime qualification remains pending; this is not permission to integrate or deploy.
The later [runtime review](2026-10-29-present-guides-runtime.md) records newly recreated baselines, bounded
present evidence and a D3D11 shutdown failure; this source/build review preserves its earlier observations.

## Scope and ownership

Base: `dd2a589108652870a702313678cf67cf8a98ced7`, with a dirty candidate worktree identified by per-file hashes.
Codex implemented the bounded extraction; the existing Claude session independently reviewed scope and the diff;
the coordinating assistant checked the result, admitted compilation/fixtures and ran final verification outside
Codex's sandbox. There was one implementation writer and one corrective pass; no nested agents were dispatched.

27 functions moved from `retoque/shaders/dlssnr/DlssNr_Dx12.cpp` to
`retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp`: capture/cloning, identity, status and bridge consumption.
The original core decreased from 7,025 to 6,360 lines; the new responsibility unit has 810 lines including its
includes and namespace boundaries. The exact symbol set is in `tests/nr-dx12-internal/present-guides-contract.json`.

Shared storage, original initializers/order, locks, local statics and public entry signatures are preserved.
Private helpers and `GuideAsk` use `DlssNr::Dx12Internal`; `NowMs` has one definition in core. `Dispatch`,
`RunPresentPass`, `EvaluateAfterUpscale`, synthesized motion, `ReleasePresentTemporal` and shutdown stay in core.
Only the fork-owned project/filter entries changed under upstream `OptiScaler/`. No INI, shader/bytecode,
model/FG feature, GPU/VRR setting or game-installation change is part of this diff.

## Review and corrections

Claude compared bodies with the base and found the extraction faithful, then identified these refinements:

- Put private declarations in the private namespace, rather than adding a generic external `DlssNr::NowMs`.
- Update affected guide/MSVC/transport/state-test READMEs and document extraction step 8.
- Restore explicit uniqueness rejection in the shared fixture assembler; missing/duplicate state or `GuideAsk`
  must not silently select a first match.
- Preserve lexical token boundaries in body hashes: `a - -b` must differ from `a--b`, and `x + +y` from `x++y`.
  Do not count a control-statement call as a function definition.
- Restore the explanatory `GuideAsk` comment beside its type rather than leaving an orphan in core.

Those items were corrected. Regression failures were observed before fixes and passed afterward. Original
state/resource contract hashes were not rewritten. The coordinator independently recomputed all 27 body hashes
from `git show dd2a5891` and checked them against the candidate, rather than trusting hashes of the candidate.
Existing semantic assertions (forbidden operations, reset/log counts and capture guards) remain active; one
previously optional clone-rejection guard was made mandatory.

Minor deferred: the new unit retains the broad original include block. No runtime defect was identified from it;
include minimization can be a separate measured cleanup. The struct bodies were compared, but the dedicated
function-hash contract is not a general C++ semantic proof or a substitute for the compiler/runtime gates.
The corrective pass was checked by the coordinator and regressions, not by a claimed second independent review.

## Verification actually performed

The clean reference worktree passed the six focused host suites and the MSVC identity fixture before the candidate.
Codex's initial sandbox runs reported 1 PASS/5 FAIL focused and 22 PASS/78 FAIL full; 73 full-run failures explicitly
reported LeakSanitizer's ptrace incompatibility. Exploratory runs with leak detection disabled were not accepted
as final evidence. No sanitizer suppression was added to the source. The coordinator ran both independent native
full passes with `ASAN_OPTIONS` and `LSAN_OPTIONS` removed; each passed all 100 host suites.

The final pass below is after the corrective changes. Per-file source hashes were identical before and after each
stage; compilation artifacts are ignored and recorded separately. The report honestly records `dirty=true`.

| Gate | Observed result | Evidence relative to execution worktree |
|---|---|---|
| Original-body comparison | 27/27 match original base, linkage/format normalization only | `present-guides-contract.json`; coordinator original-source check |
| Full native host tier, four workers, strict | 100 PASS; zero aggregate FAIL/SKIP/TIMEOUT/ABORTED/INCONCLUSIVE | `artifacts/present-guides-final/host/summary.json` |
| clang-format 22.1.8 | All three changed handwritten C++ files passed | `artifacts/present-guides-final/format.log` |
| Documentation and catalog | Local destinations/catalog passed | `artifacts/present-guides-final/docs.log` |
| Whitespace/diff | Passed | `artifacts/present-guides-final/diff.log` |
| Release MSVC under Wine, four compile workers | Exit 0; DLL and forwarder produced | `artifacts/present-guides-final/build.log` |
| MSVC identity fixture under Wine | PASS: 1,235,154 identity checks and 253 glue checks | `artifacts/present-guides-final/msvc/nr-present-guide-identity-msvc.log` |
| D3D11/D3D12 real-device transport under GE-Proton11-11 | Eight required cases crossed byte-exact | `artifacts/present-guides-transport-final/dx11-guides-transport.log` |

Final host invocation: `1ed888dcf4b141cb8f678ef1bed9cec0`.
MSVC fixture invocation: `4e4bce60c37f4fab8b1b08aa3fe0b9a6`.
Transport invocation: `eab25e7f99f740c3a2dd17551ef42542`.
The staged command/exit/timestamp record is `artifacts/present-guides-final/stages.json`.

Important boundary: the MSVC fixture executes real extracted glue with fakes, not D3D12. The real-device
transport fixture executes the transport's API sequence in its own harness; it does not load the candidate
OptiScaler DLL. Neither validates the candidate DLL's actual game/present path, frame pacing or visual output.

## Retained binary and input identities

Original DLL (29,284,352 bytes):
`687cdbe2adf574321dfb3f03fe5e4964741bf8a2b4cd0a5f5408314006d03f04`.
Original forwarder:
`68f78e20ebf2e150647c66c584631a7ddcb86c4346566260ae0ecbaba178c8ad`.
The existing original build stamp is `da751da7`; its exact source-to-binary provenance was not independently
proven and it was not relabeled as a clean `dd2a5891` build.

Candidate DLL (29,285,376 bytes):
`258ebbab84f6a8558750410046da781bb773b12225d53cfb98a443c019ccde40`.
Candidate forwarder:
`6b19e45a5e736bfbbf1e735d33855ced8b532e529c85f22220c304c8695be0b6`.
Candidate source-snapshot file SHA-256:
`bfaef39fa863c16c23dccf862d5dd27c8eb6787626822f5b27e9b22cd6a07f15`.
The candidate's HEAD stamp does not encode the uncommitted diff; use the snapshot and DLL hashes, not the stamp
alone. Final review/plan prose was written after verification without changing tested code or test fixtures.

Local evidence retention root: `<evidence-root>`.
It holds `binary-before/`, `binary-after/`, receipts and a retained 33-frame RR input recording in `corpus/`.
The recording copy has 100 files and 3,269,425,435 bytes, each compared by SHA-256 with its original; the original
was not moved or deleted. At this review stage the input recording was not a frozen output baseline; the old
frozen replay baselines were absent, so no old/new output-equivalence result was then claimed. New baselines
were subsequently recreated and compared as recorded in the later runtime review. Original main-checkout DLLs were unchanged.

## Remaining qualification / integration boundary

At this source/build review stage, original-DLL output baselines and candidate present/D3D11 runs were still
pending. The later runtime review records the subsequently recreated baselines and bounded route runs, including
the D3D11 shutdown FAIL. After-upscale replay remains a control for that route; switching/lag, resize, final
present pixels and scoped native Windows validation remain open. No game was launched or modified for this task. Do not promote this source/build
success to image-quality, performance, native-Windows or universal game approval.
