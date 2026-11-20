# Retoque roadmap

Reviewed 2026-10-29 against source `9778aeb5`. This document owns priorities and blockers, not compatibility.
Use [capabilities and evidence](../capabilities.md) for implementation/validation state. A candidate below is not
permission to implement it or change defaults. The original comparison is preserved in the
[dated roadmap snapshot](roadmap-20261029-history.md).

## Active organizational work

The approved repository-clarity steps preserve source paths and renderer behavior: short agent instructions,
contributor/architecture guidance, an evidence-aware capability index, domain-based test discovery and release
identity linked to source tests. See the [scope](../superpowers/specs/2026-10-29-repository-clarity.md) and
[implementation plan](../superpowers/plans/2026-10-29-repository-clarity.md). Renderer extraction is not part of them.

## Validation before more runtime changes

Record current-build runs for the instance guard, display-only adapter filtering and opt-in zero-motion probe.
The code landed in the `da751da7` integration, but merge/build status alone does not prove those runtime scenarios.
Use exact installed DLL hashes and the route-specific [validation matrix](../maintenance.md#validation-route-matrix).
Do not overwrite a newer field record with this planning snapshot; link the new record in the capability index.

## Candidates, in proposed order

| Priority | Candidate | Next decision / evidence needed |
|---|---|---|
| 1 | Revisit ViT reuse | Reconcile [vit-skip](design/vit-skip.md) with the newer reference implementation; profile on target hardware, especially Ampere, before a port. Include cuts, resize and total-frame cost |
| 2 | Adaptive LSFG multiplier | Establish presenter's runtime baseline first; measure real FPS, latency and pacing, not only generated FPS. Respect V-Sync target limits without changing VRR. A log-only [shadow](design/lsfg-adaptive-shadow.md) (phase 1, 2026-10-30 branch) records that baseline; its runtime traces come before any acting phase |
| 3 | Evaluate RR scale 0.75 on Ada | Extend the [retention study](design/working-scale-detail-retention.md) beyond one clip; A/A calibration, visual review, non-RR and varied motion. Pin and record the RR provider and preset: SDK 310.9.1 made RR Preset F the default. Defaults stay unchanged |
| 4 | Additional temporal / private-reconstruction ideas | Compare against existing Stage/Transfer behavior and [rejected measurements](design/look-model-half-rate.md); only a demonstrated gap earns a new experiment |

## Legacy API coverage proposal

32-bit executables, D3D10 and OpenGL: [design under review](design/legacy-apis.md), 2026-11-20.
Proposed first step is a default-off, metadata-only diagnostic with no hooks, GPU work or visible change.
Then decide between qualifying existing Windows window capture and measuring x64 D3D10/DXVK reachability
through the Vulkan host. Native adapters, an x86-to-x64 helper and native Linux GL remain separate proposals.
Owner-selected games, output mode and latency budgets are pending; no implementation is approved by this entry.

The [common frame-transport proposal](design/frame-transport.md) adds an unwired host-tested protocol core
(2026-11-20), not a renderer or supported route. Next gates are authenticated x86/x64 transport fixtures,
a separately reviewed offscreen NR seam, and native per-adapter lifetime/copy/latency evidence. D3D10 relay
can exceed the one-copy-per-direction bar; Wine GL direct D3D12 imports are not assumed. Existing window-host
behavior and capability claims stay unchanged. No runtime integration is authorized by this roadmap entry.

## Implemented paths with remaining coverage work

Native D3D11 guide transport is not still waiting for its first implementation: step 3 and the historical
Sons Of The Forest observation are in [dx11-native-dlss-guides](design/dx11-native-dlss-guides.md).
Next work is broader cost/quality/environment coverage, not repeating the obsolete first-run task.

The Vulkan present host likewise has cold NR and later route/guide work. Follow the current
[host design](design/nr-vulkan-present-host.md); FG coexistence and each format/queue path have their own boundaries.
The former stage-2-only table is historical, not the current implementation state.

External sm86 operation has actual target-hardware field records, including a 2026-10-29 RTX 3060 run.
That does not finish the separate native Retoque provider plan; see [mfg-rtx20-30](design/mfg-rtx20-30.md).
Windows fixture runs, native Windows game runs and a release's binary qualification remain distinct.

Diagnostic export is implemented ([diagnostic-export](design/diagnostic-export.md)). Next work is a current-binary
in-game check of "Copy NR diagnostics": detected versus used inputs, pending/stale timing, NR off and redaction.
Host coverage does not validate the clipboard result in a running game.

## In progress

Character inspection and per-object rules, reopened by the owner 2026-11-19:
[character-inspector](design/character-inspector.md). The source audit distinguishes NeuRotic's active detection/
overlay rules from its inactive regional NR requests. **Slice 0 is implemented (2026-11-20):** `MaskInspection=log`
says in the log which protections a recorded D3D12 present frame was sent with (UI alpha handed or why not, each
pass's AutoMask and SkinStructure, the composition's guard, residual limit, highlight recovery and darkening floor);
off by default and inert, no pixel read or changed, no menu control. Host contracts only
([nr-mask-inspection](../../tests/nr-mask-inspection/README.md)); the integration build and a game run on the
present route are pending, and the note lists what that run has to show. Next, still under review: a view of the
actual UI mask and composition weights on the existing debug-view surface. No detector or regional image change is
authorized. This supersedes the earlier decision not to promote character inspection merely for reference-fork parity.

Real depth without an upscaler, reopened 2026-11-19: [step 0](design/present-depth-without-upscaler.md) is a
D3D12 `PresentDepth=log` observer only, with independent ranking/refusal/comparison host contracts. No depth is
supplied to the model. The four diagnostic-contract amendments were accepted by the lead, owner confirmation
at review. Integration build and in-game numbers are pending; step 0b is described, not built, and step 1 waits
for both precision/coverage and demonstrated value.

NR on any window ([nr-anything](design/nr-anything.md)): Windows-only capture host, approved with no upscaling or depth estimation. Phases 0-2 (probe, DLL seam, preview) were built and host-tested but never run on Windows. Phase 3 (2026-11-19) is implemented: opt-in click-through DComp overlay, pause/fresh-frame visibility rules, hotkey and recorder exclusion off by default. Host contracts are built/tested and Win32 units syntax-checked with MinGW; the integrated MSVC build and Windows/RTX 3060 checklist remain pending. No compositor, input, latency or GPU qualification is claimed. Next: integration build, native preview/overlay checklist, then the app screen (phase 4).

## Parked or deliberately not promoted

`Stage=1` is not promoted to the default from a single [before-upscale measurement](design/stage-before-upscale-measured.md).
The tested adaptive cadence and half-rate look model were not promoted; preserve their negative results.
The guide-fidelity probe is designed and parked, not implemented. Its temporal pairing, ordering, source continuity
and recording/readback lifetime contracts must close before implementation; see
[guide-fidelity-probe](design/guide-fidelity-probe.md). It supplies no current guide-quality verdict.
Other unrelated capture features and closed reference DLLs are not adopted simply because another project
advertises them. Character inspection moved to design review above on 2026-11-19 (slice 0 built 2026-11-20). Ten-pass chains were parked
on that ground until the owner asked for them on 2026-11-11:
`Passes` now goes to ten (default one, RR route still three), with the cost said under the control and in
[bounded-chain](design/bounded-chain.md) ("Ten passes"); its memory and time at ten on an RTX 3060 and a 4090 are
not measured yet.

## Updating priorities

Every promoted candidate needs a bounded design, explicit off-path contract, acceptance scenarios and evidence.
Record rejection/parking reasons. Move resolved tasks out of the active table, link the capability/evidence entry,
and retain dated history instead of maintaining contradictory "done" and "next step" copies.
