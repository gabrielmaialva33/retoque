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
| 2 | User-facing diagnostic export and effective-state explanation | Reuse current NGX/route/guide facts; separate detected inputs from inputs actually used. This is a future in-game feature, not the release sidecar added by repository tooling |
| 3 | Adaptive LSFG multiplier | Establish presenter's runtime baseline first; measure real FPS, latency and pacing, not only generated FPS. Respect V-Sync target limits without changing VRR. A log-only [shadow](design/lsfg-adaptive-shadow.md) (phase 1, 2026-10-30 branch) records that baseline; its runtime traces come before any acting phase |
| 4 | Evaluate RR scale 0.75 on Ada | Extend the [retention study](design/working-scale-detail-retention.md) beyond one clip; A/A calibration, visual review, non-RR and varied motion. Pin and record the RR provider and preset: SDK 310.9.1 made RR Preset F the default. Defaults stay unchanged |
| 5 | Additional temporal / private-reconstruction ideas | Compare against existing Stage/Transfer behavior and [rejected measurements](design/look-model-half-rate.md); only a demonstrated gap earns a new experiment |

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

## Parked or deliberately not promoted

Real depth without an upscaler remains gated on the value experiment in
[present-depth-without-upscaler](design/present-depth-without-upscaler.md), not on implementing a scanner first.
`Stage=1` is not promoted to the default from a single [before-upscale measurement](design/stage-before-upscale-measured.md).
The tested adaptive cadence and half-rate look model were not promoted; preserve their negative results.
Ten-pass chains, unrelated capture/character-inspector features and closed reference DLLs are not adopted simply
because another project advertises them.

## Updating priorities

Every promoted candidate needs a bounded design, explicit off-path contract, acceptance scenarios and evidence.
Record rejection/parking reasons. Move resolved tasks out of the active table, link the capability/evidence entry,
and retain dated history instead of maintaining contradictory "done" and "next step" copies.
