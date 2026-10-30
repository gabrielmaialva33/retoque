# Adaptive LSFG multiplier, phase 1: a log-only shadow

Status: **implemented on `feat/lsfg-adaptive-shadow-20261030` (2026-10-30), host-tested only, not run in a game.**
Roadmap [candidate 3](../roadmap.md). Phase 1 changes no frame count and no pacing: it adds one log line to the MFG
presenter's 10 s summary saying what an adaptive policy would pick, so the runtime baseline the roadmap asks for can
be recorded before any policy acts.

## Off-path contract

- LSFG off, FFX's own swapchain (x2 latched), any other FG: no code of this change runs.
- Our presenter running (`MfgSwapChain_Dx12`): `setGeneratedFrames`, the count `dispatchInterpolationCommands`
  reads, the router's calls and every `presentQpcDelta` are byte-for-byte as before. The shadow is fed on the
  presenter thread after an entry's presents and read in the summary; it has no path back. `tests/lsfg-adaptive`
  holds this in the source.
- Its costs on the presenter thread: a few arithmetic operations per entry; `Util::GetActiveRefreshRate`
  (`MonitorFromWindow`, `GetMonitorInfoW`, `EnumDisplaySettingsW`) at the first entry, every 2 s while the rate is
  unknown, otherwise once per summary; `Config::FramerateLimit` read once per summary; one more `LOG_INFO`.
- No config key, no menu control, no VRR interaction. The display's VRR setting is neither read nor changed.

## What it computes (`retoque/lsfg/presenter/MfgAdaptive.h`)

Reimplemented, not copied, from MAKO (eugeniosegala/MAKO at `a321c2a`, GPL-3.0-or-later,
[attribution](../../../Licenses/MAKO_ATTRIBUTION.txt)), with MAKO's constants:

| Piece | MAKO source | Rule |
|---|---|---|
| `RefreshBudget` | `FixedRefreshBudget::plan`, `presentation_policy.hpp:1642-1710` | interval EMA 0.75/0.25; credit += max(interval × refresh, 1); generate min(floor(credit) − 1, max); carry ≤ 1 output; first frame, a backwards clock or a gap over 250 ms: 0 and start over; refresh unknown: max |
| `CadenceEstimator` | `AdaptiveScheduler` cadence observation, `adaptive_scheduler.cpp:668-827` | interval EMA; exclude bursts over max(3 × baseline, 2 × target) fps and stalls under 10 fps; a 2× interval is held in a shadow estimate, confirmed as a drop on the third in a row (rebase), adopted as a hitch otherwise |
| `DeliveryWindow` | `GeneratedDeliveryWindow` | healthy while misses ≤ requested / 20 (integer) |
| `HigherLoadIsUseful` | `adaptiveHigherLoadIsUseful` | output (capped at the target) gained ≥ real fps lost + 2% of the target (x3 up); a delivered reach of the target from under it waives the cost |

Ours, not MAKO's: the target is XeFG Dynamic MFG's auto target (`XeFG_DynamicTargetFps(0, FramerateLimit, refresh,
V-Sync)`: just under the refresh, held to the FPS limit, and to the refresh under V-Sync); measurements at another
count expire after 60 s; the refresh is re-read as XeFG does (`XeFG_DynamicMFG::DisplayRefreshFps`) but no more than
once per summary once known, to keep Win32 calls off the presenter thread's hot path.

`Advise` reports two choices, generated frames 1..3 (x2..x4), each with a reason:

- **XeFG-style**: the count whose presented rate ((n + 1) × real fps) lands nearest the target, `XeFG_DynamicCount`'s
  "around" rounding in one shot. It has no hysteresis, learned frame cost or post-change check: those need the count
  applied, which phase 1 never does.
- **MAKO rule**: delivery pressure steps down; a measured lower count that the current one does not beat net of its
  real-frame cost steps down; the target met holds; a measured higher count that pays steps up; otherwise it holds
  and says one step more is untested. A projection never claims a step.

Evidence for a count is the cadence estimate at the end of a *steady* window: one count throughout, healthy delivery,
no drop or stall. Since nothing changes the count in phase 1, evidence for other counts appears only when the user
changes `[LSFG] Multiplier` live, so the MAKO column will mostly read "one step more untested". That is the honest
answer, and the reason phase 2 needs trials.

The presenter feeds it per entry: the real-frame time is `PacingData::handoffQpc` (the game's `Present`); an entry
dropped before presentation shows as a gap in the present indices, counted as missed frames, and its interval is no
cadence sample. The refresh budget's maximum is the count the entry actually carried, so "budget B of G" reads as how
many of the generated frames the refresh could show.

## The log line

After the two existing summary lines (MFG_PRESENTER.md section 6):

```
LSFG presenter: last 10.0 s: adaptive shadow (log only): refresh 165 Hz, V-Sync on, target 157.0 fps; real cadence
60.1 fps (bursts 0, stalls 0, hitches 1, drops 0); refresh budget 1048 of 1202 generated (1.74 per real); delivery
1202/1202 (healthy); running x3, steady; XeFG-style x3 (nearest the target), MAKO rule x3 (one step more untested)
```

(Illustrative layout; values above are not a measurement.)

## Host evidence

`python3 tests/lsfg-adaptive/run.py`, 2026-10-30, uncommitted worktree on `2d36321b`: 1690 checks, 0 failures,
source guards ok. MAKO's own vectors: 570 of 630 (63 fps, x2, 120 Hz; MAKO expects 565-575) and 120 of 600 (100 fps;
115-125). The guards were mutation-checked: changing `setGeneratedFrames`, adding a writer of
`generatedFramesWanted` or `presentQpcDelta`, touching the shadow from the interpolation thread, feeding it before
the presents, or changing the router's call each fail the suite.

## Before phase 2 (acting on the advice)

Runtime evidence, current build, exact DLL hash, per the [validation matrix](../../maintenance.md#validation-route-matrix):

1. Baseline traces of the shadow line at x3 and x4 with V-Sync on at 60, 120 and 144/165 Hz, V-Sync off, and an FPS
   limit, on a GPU-bound and a CPU-bound title; with each, the existing present-interval p50/p99, share over 1.5 ×
   target, and the "presented after the game's Present" latency.
2. Whether `handoffQpc` cadence matches an external frame-time capture (MangoHud/PresentMon), including entries
   dropped before presentation, and how often they occur.
3. Whether the refresh budget's "B of G" matches frames actually lost under V-Sync (does the presenter hold the game
   or drop generated frames?). The budget only helps if non-integer generated counts per entry pace well; that needs
   a per-entry count path, which does not exist and would change pacing.
4. The cost of `EnumDisplaySettingsW` on the presenter thread, measured, before sampling more often.
5. Live multiplier changes (x3 ↔ x4) to give the MAKO rule evidence, checking that the measured real-fps drop per
   step matches what it would decide. Only then design trials, hysteresis and an off-by-default control
   (config key round-trip, menu, shipped INI), with native Windows validation for any pacing change.
