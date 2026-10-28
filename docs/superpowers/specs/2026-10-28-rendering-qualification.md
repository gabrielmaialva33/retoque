# Rendering qualification: environment, evidence and temporal coverage

## Intent and authorization

Implement the four-item next round accepted in the conversation: fail-fast graphical diagnostics, complete bounded replay evidence, a representative opt-in temporal corpus, and a permanent Vulkan WorkingScale=1.25 regression. Baseline source is f8d9859aedff628bb0d65429a7a55b360294787a. This is test infrastructure and qualification work; the production renderer and shipped settings remain unchanged.

## Approach

Extend the existing serial rendering entrypoint, replay harness and Vulkan route fixture. Reuse their result identities, lifecycle supervision, analytic scene and validation gates. A separate test framework would duplicate those contracts. Widening visual tolerances would not address the missing scenario coverage. These implementation choices are the builder's within the accepted scope.

## Constraints

- Work on an isolated branch until measured; no git push or game deployment.
- Root alone owns Winx and all remote mutations. Agents work on canonical local snapshots with disjoint file ownership.
- Wine/compiler/GPU work is serial. Reuse existing locally supplied model and dependency checkouts.
- No production shader, renderer, cadence threshold, Stage default, model or shipped INI changes.
- Preserve existing short replay behavior: 256 frames, 160 warmup, logical 120 Hz.
- Pixel-policy failures remain visible and distinct from functional seam success. No perceptual approval is inferred.
- Never weaken the existing Vulkan model VUID allowlist or suppress host errors.
- Durable qualification output is per invocation, terminal, hash-bound and honest about missing coverage.

## 1. Graphical preflight

The full rendering plan begins with a supervised X11/XWayland connection check. It uses the inherited DISPLAY exactly, via XOpenDisplay/XCloseDisplay from the system library. It does not select a desktop, modify global environment or claim native Wayland/GPU coverage. Missing DISPLAY, library or authorization is a terminal non-PASS. The deadline is 8 seconds, with the runner's existing owned cleanup and interruption semantics. --host-only omits this step.

The helper writes the existing versioned suite receipt with the current invocation identity and runtime_coverage=false. The orchestration validates it and excludes the preflight from DLL requirements and runtime coverage. Subsequent steps remain NOT_RUN when it fails.

## 2. Replay evidence retention

For the replay step, preserve fresh artifacts under judge/: calibration.json, policy.json, corpus.json, and each reported variant's result.json, frames.jsonl, contact-sheet.png and diff-heatmap.png. Optional moving-plan.txt may be retained as the fourth root artifact. Store bytes and SHA-256 of actual retained content.

Inventory allowlisted sources before execution. Unchanged old files are STALE and cannot stand in for current evidence. Never erase previous diagnostics to manufacture freshness. Missing required current evidence, copy failures or bounds violations make evidence retention FAIL and prevent overall qualification PASS, while retaining the original functional and numerical verdicts.

Bounds: at most 32 variants, 132 files, 16 MiB per file, 128 MiB total; summary/configuration JSON at most 2 MiB. Do not traverse raw-frame trees or follow file/directory symlinks. Verify opened-file identity and bounded copies. Reports expose exclusions and limits.

## 3. Temporal replay profile

Add --replay-quality to the loopback entry and consolidated entry. It selects temporal-v1 in a separate artifacts/replay-quality-run directory. The default --replay-nr remains short. Temporal qualification rejects custom captures and contradictory frame/warmup overrides rather than mislabeling them.

temporal-v1 uses 480 frames at 60 Hz (8 seconds nominal, 400 scored frames). The first 80 are warmup; scoring thereafter is continuous, as required by the current capture contract.

| Frames | Phase | Pose/material |
|---|---|---|
| 0-79 | warmup | fixed; reset only at 0 |
| 80-159 | steady | fixed |
| 160-239 | motion | advance two PoseForFrame steps per frame |
| 240-319 | recovery | hold the last actual pose |
| 320-359 | fast-motion | advance twelve pose steps per frame |
| 360 | cut | distinct pose; reset=true; previous=current |
| 361-479 | post-cut-recovery | hold the cut pose |

A versioned moving plan carries frame ordinal, current/previous pose indexes, reset, logical time and delta. The test harness uses existing PoseForFrame for both states and freezes material frameIndex in holds; jitter follows rendered ordinal. No fixture shader changes. Outside a reset, previous pose must equal the preceding rendered pose. The harness returns matching metadata, checked before capture construction.

The cut is scored. Verify its existing reset semantics before claiming coverage; do not hide skips or add a second warmup. Keep startup stabilization, cadence and quality gates intact.

Check in an explicit v1 numerical policy with the existing defaults: min_coverage=0.25, pixel_tolerance=0.01 and all eight ERROR_METRICS floors=0. Pass the same policy to calibration and candidate judging. Preserve policy bytes/hash and corpus/profile metadata in judge/. Explain that the comparison is against an A/A envelope, not a perceptual acceptance policy.

Report nominal duration, timestamp span, scored count/duration, phases, resets and observed model/carried/skipped decisions and intervals by phase. Inputs remain a rendered synthetic DLAA corpus with analytic guides. HDR, exposure changes, DRS, game captures and controller tuning are later work.

## 4. Vulkan scale regression

Register nr-route-scale125 in ROUTE_CASES and require it in the consolidated VK_CASES. Reuse nr-route-quiet's complete --route scenario and INI, adding only WorkingScale=1.25. The reference manual INI does NOT set ApplyModel=false: preserve the existing route's behavior.

Keep all eleven ownership/hand-back phases, active route evaluations 30/30/10, exact active-phase readbacks 28/28/8, and the existing cold/hand-back model checks. Require three route builds with frame 1280x720 and working 1600x900, successful active evaluations, no scale fallback and active host validation with zero errors. Preserve only the already admitted model VUID-vkCmdDraw-None-09600 within the existing count contract. VUID-vkCmdDispatch-OpImageWrite-08795 fails in every attribution.

Coverage remains source-backed supersampling up-leg execution and existing route behavior. It does not independently prove down-leg sampling, pixel algorithm correctness, exposure readback, command-buffer resubmission or visual quality.

## Acceptance

Host tests exercise real Python interfaces, temporary files and supervised child processes, with meaningful negative controls. A real inherited-display preflight, the registered scale case and the temporal replay run must produce retained invocation-bound evidence. Reuse the already qualified DLL pair because production source is unchanged; rebuild the changed test harness. Record the precise commands, results, hashes and remaining limitations before integration.
