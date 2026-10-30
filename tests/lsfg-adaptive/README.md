# `lsfg-adaptive`

The MFG presenter's log-only adaptive multiplier shadow, phase 1 (`retoque/lsfg/presenter/MfgAdaptive.h`, design in
[lsfg-adaptive-shadow](../../docs/dlssnr/design/lsfg-adaptive-shadow.md)). The rules reimplement MAKO's
(GPL-3.0-or-later, `Licenses/MAKO_ATTRIBUTION.txt`); the vectors restate MAKO's own test expectations.

## What it covers

1. **`MfgAdaptive.h`**, compiled unmodified with the real `XeFG_DynamicCount.h` under ASan/UBSan (`test.cpp`):
   - refresh budget: 63 fps at x2 on 120 Hz budgets 565-575 generated frames of 630 and 100 fps 115-125 of 600
     (MAKO `presentation_policy_tests.cpp:1702-1725`); refresh unknown gives the maximum; the first frame, a clock
     going backwards and a gap over 250 ms generate nothing; at most one output of credit carries; at 165 Hz with
     V-Sync, 60 and 70 fps at x4 get 1 or 2 generated frames per real frame, totalling the displayable remainder;
   - cadence estimator: an alternating 8/32 ms trace reads 40-60 fps; a burst over max(3x baseline, 2x target) is
     excluded and one under it is not; one or two 2x intervals are a hitch, the third confirms a drop and rebases;
     intervals over 100 ms or backwards are stalls; `Rebase` skips an interval without a sample;
   - delivery: at most 5% missed, integer (under 20 requested, none may miss);
   - acceptance: MAKO's 40→30 FPS (80→90 out) trade at a 120 target is rejected and 40→32 accepted
     (`adaptive_scheduler_tests.cpp:2990-3020`); output capped at the target; delivered target evidence waives the
     real-frame cost only from under the target;
   - target: XeFG's auto target, the FPS limit, and min(target, refresh) under V-Sync;
   - `Advise`: XeFG-style nearest presented rate and MAKO's rule over measured loads, with every reason, always 1..3
     generated frames;
   - `Shadow`: refresh sampled at once, every 2 s while unknown, otherwise at a summary 2 s after the last read;
     entries dropped before presentation count as delivery misses and are no cadence sample; evidence only from
     steady windows (one count, healthy, no drop or stall) and only for 60 s.
2. **Source guards** (the adviser only reads):
   - `setGeneratedFrames` and the count `dispatchInterpolationCommands` reads are byte-for-byte as before;
     `generatedFramesWanted` has one writer; the router's two `setGeneratedFrames` calls are the configured and the
     live multiplier; the swapchain never calls it on itself;
   - the shadow is not touched by the interpolation thread, `presentInterpolated`, `dispatchInterpolationCommands`,
     `setGeneratedFrames` or the router; in the presenter thread it is only fed (`OnEntry`), after the entry's
     presents; the summary closes it; the refresh is read through `Util::GetActiveRefreshRate` as XeFG does;
   - `presentQpcDelta` still has its one writer, `PresentDelta` in the interpolation thread;
   - `MfgAdaptive.h` needs no Windows, `Config` or `Util`, takes the target from `XeFG_DynamicTargetFps`, and carries
     MAKO's attribution, as does `Licenses/MAKO_ATTRIBUTION.txt`.

## What it does not cover

The D3D12 presenter, a display, V-Sync, VRR or a game. Whether the logged advice is right needs runtime traces (the
design note lists them). The guards check text, not behaviour; the MSVC build is not exercised here.

## Run

```bash
python3 tests/lsfg-adaptive/run.py
```
