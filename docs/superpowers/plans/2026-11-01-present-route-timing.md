# D3D12 Present route and guide-capture timing

> Implementation plan. User authorized execution on 2026-11-01 ("Vamos la"), following the engine-inputs/upscale/Present study. No commit, merge, push or game deployment is part of this change.

## Goal and scope

Measure the GPU commands in the D3D12 RunPresentPass route, from its first guide clear or backbuffer copy through the final backbuffer restore, with the existing opt-in GpuTiming setting. Publish the actual guide source, capture sequence and age as immutable sample metadata. Separately measure guide-copy commands on their producer list. Preserve rendering, synchronization, configuration defaults and the existing Dispatch/Conversion measurements.

This is GPU queue elapsed time. CPU allocation, command recording, submission and the existing PresentSync wait are outside the GPU bracket. It is not FPS, end-to-end latency or pure kernel utilization. The native Vulkan and D3D11 Present hosts are outside this first route bracket; a D3D11 producer reaching the shared D3D12 guide capture may produce a guide-capture sample.

## Architecture

Add a two-timestamp Region scope to DlssNr_GpuTiming, with independent bounded pools (eight slots per region), counters and sampling phases for Present and GuideCapture. Reuse Allocate/ReleaseSafe, Submission::TimingCertificate and DecodeBrackets. The existing sixteen Evaluation/Conversion slots remain available exclusively to them.

Each accepted sample requires the existing sealed, single-execution, completed-fence certificate and actual queue frequency. Unknown or replayed recordings are never mapped speculatively. Disable releases only safe idle allocations; retained pending recordings remain bounded. No new wait, queue flush, hook implementation or rendering policy. Track may activate the existing submission hook and its normal post-execution Signal.

Region metadata owns values: observed Present ordinal (not an engine frame ID), ring capture sequence (zero when absent), capture age at selection, guide source enum, boundary entry, output/guide extent, HDR and whether the NR evaluation recorded work. Present includes nested Dispatch and HDR conversion; these samples must never be added to its total. Guide capture belongs to its producer and is never automatically added to a Present sample.

The UI and diagnostic export label these as independent historical confirmed samples, show their actual provenance and distinguish unavailable samples. INFO records carry queue/fence proof and per-kind counters.

## Work and verification ledger

- [x] Inspect current branch/worktrees and repository rules; create isolated feat/nr-present-timing-20261101 at 73fad086.
- [x] Baseline: nr-gpu-timing (31 portable, 3000 epochs, 24 runtime, 6 cadence, 6 conversion), nr-timing-boundary (31), nr-diagnostic-report all pass.
- [x] Add behavioral runtime cases first; observed missing-API failure against old code, then 13 cases green.
- [x] Implement Region lifecycle, isolated pools, certificate collection and immutable metadata.
- [x] Instrument RunPresentPass and CaptureGuides; close scopes before Close/Drop/Submit, including failures.
- [x] UI/export RED then GREEN (36 metadata/UI cases plus diagnostic report); observable states documented. No new test registry entry.
- [x] Final strict selection: 9/9 host suites PASS; clang-format 22.1.8, diff and documentation checks PASS; Release DLL/forwarder built under MSVC/Wine.
- [x] Real D3D12 timing harness PASS on the adapter identified as NVIDIA GeForce RTX 4090; replay, blocked queue and negative coverage controls passed. Native Windows and in-game/NGX behavior remain outside this fixture.
- [x] Independent source-packet lifetime/placement/UI review complete, no material defects found; final evidence recorded in the matching review document.

## Review focus

Check nested scopes on one recording, no ticket/slot collision, no pool starvation of Dispatch, sampler phase independence, no map before both reset seal and fence completion, replay/device-loss retention, lock order (NR -> timing -> Submission; hooks never acquire timing), default-off inactivity, double Finish/destructor behavior, and newest-recorded sample winning over completion order. Actual guide provenance is selected from resources used by the route, not the temporal-valid latch. Capture age is historical at selection; no synthetic engine frame identity.

Use the existing host runtime with only external D3D12 calls faked, not a second implementation. Add renderer placement checks only where the host cannot execute the Windows renderer; build and review remain required. Run targeted suites once green, expanding only for a concrete remaining risk. Repository rules override skill boilerplate about commits or additional permission prompts.
