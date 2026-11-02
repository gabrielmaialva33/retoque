# RTX 4090 cadence optimization: source review

Date: 2026-11-01. Base: `73fad086`; candidate: uncommitted
`perf/rtx4090-cadence-20261101`. This record covers source review and build/host gates.
Runtime qualification is recorded separately in the
[dated measurements](../../dlssnr/design/cadence-deferred-coarse.md).

Design: [deferred coarse](../specs/2026-11-01-rtx4090-cadence-design.md).
Execution: [plan](../plans/2026-11-01-rtx4090-cadence.md).

## Reviewed change

- Extract `CadenceAccept` from the carry rule and use it for the adaptive probe.
  The probe receives null residual/coarse descriptors and does not read either texture.
- Move the unchanged coarse reduction from `Record` to the beginning of `Carry`
  when the existing `InChainStart` is true. Subsequent carries and Fade reuse it.
- Preserve scheduling, recorded residual/proxy/depth, allocation and retirement,
  resource transitions, descriptor capacity, constant layout and settings.

An independent reviewer read the production diff, scheduler/callsites, resource setup,
the rule's dependency chain and new regression sources. The controller checked the
final descriptor-budget comment separately.

## Review findings and resolution

1. **Partial Carry recovery was initially missing from the new fixture.**
   The revised fixture fails Show after chain/synthesis have run, calls the production
   scheduler's `Drop(true)`, records a distinct new history and checks its first carry
   against eager coarse and half-rounded output. The existing early refusal and partial
   model failure cases remain.
2. **Initial probe comparisons used mostly still, fully accepted samples.**
   A direct production `CadenceAccept` versus frozen `LegacyCarryEdit` comparison now
   covers fractional motion, occlusion, no-match regions, borders and NaN/Inf
   displacement. It requires rejected, partial and fully accepted populations and
   checks residual/coarse read counters around the new rule alone.
   The Fade reference also uses the frozen carry, avoiding a shared acceptance oracle.
3. **Minor cleanup.** The runner keeps its normal temporary-directory behavior;
   execution sets `TMPDIR` instead. The header comment now explains the six-dispatch
   maximum while retaining all 112 slots.

Final source verdict: **ready for GPU validation**, with no remaining Critical or
Important findings. Coarse depends only on the stored residual and dimensions;
its use of the Carry frame's constants introduces no motion/depth/time dependency.
The source paths preserve Fade-before-Record and first-carry recovery after Drop.

## Verified host and build evidence

- Targeted cadence: 21 scheduler cases, 22 rule cases, adaptive tests and the production
  orchestration fixture passed under ASan/UBSan.
- Full host invocation: 121 PASS and one prerequisite failure, `nr-drs-override`,
  because the isolated worktree initially lacked `NvApiDriverSettings.h`.
  With `external/nvapi` populated at its exact gitlink revision, the single-suite retry
  passed. All 122 selected suites therefore have passing evidence; the first full
  invocation is not described as a clean PASS.
- clang-format 22 check and documentation check passed.
- DXC regeneration succeeded. Generated header bytes were checked against the CSO.
- `./build-local.sh Release` exited 0 and produced both DLL and forwarder.
  Build warnings were retained in the log; no failed build was deployed.

Candidate OptiScaler SHA-256:
`a445fe1c53d13bf030606225b252763472eacafd3d8e6086f09340d8e55a1700`.

Generated CSO SHA-256:
`bb4f8aa0b1e74dd671533585402c234b9fee793fa6f7ca4d01ff2f1fa04ba4f6`.

## Runtime follow-up and evidence boundary

After source approval, regenerated DXIL and the Release DLL passed the same-input
GPU replay: five variants had identical output bytes and decisions on all 93
frames. A fresh baseline A/A repeat matched every-frame and fixed C4 as well.
Independent review of the game measurements recommended local acceptance for the
current adaptive configuration: roughly 0.04 ms less auxiliary GPU time on model
frames, with the approximately 0.1 ms first-carry cost shift explicitly measured.
No overall FPS benefit or fixed-cadence improvement was established.

The [measurement note](../../dlssnr/design/cadence-deferred-coarse.md) preserves
sample counts, unequal paging, window differences and runtime-identity limitations.
The host fixtures do not replace GPU or driver validation. Native Windows and
universal image-quality/performance qualification remain outside this local Proton
validation. No Critical or Important source finding remained at handover.

The generated header keeps the existing generator's trailing whitespace; it is excluded
from formatting. Non-generated source was checked separately. No commit, merge or push
was performed as part of this work.
