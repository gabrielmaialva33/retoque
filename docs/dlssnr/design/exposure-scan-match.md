# Exposure scan: choosing the candidate by matching it to the light (`[DlssNr] ScanMatch`)

Status 2026-10-30: implemented behind an opt-in key, off by default; host-tested only. No game, GPU or frame-time
measurement has been made with it on.

## Source and credit

The rule is adapted from janblade's OptiScaler-F5-DLSSNR-Multipass (GPL-3.0), `DlssNr_ExposureMatch.h` at commits
`0a80f61d`, `3e25cc36`, `0357684c`, `b003b7e8` and the easing of `32514f7e`. The fit, the rejections and their
thresholds are his, measured on Cyberpunk 2077 (r 0.986 over 9.6 EV), Doom Eternal and NBA 2K27 recordings; trimmed
copies of those recordings are `tests/nr-exposure-match/data/` (columns cut, rows unchanged, provenance in each header).

## What it does

Off, the scan offers the moving candidate with the widest travel (`BestLocked`), unchanged. On:

- **Meter.** The pulse pass's reduction (`DlssNr_Pulse.h`, `MeanInputLuma`: luminance in over pixels, the game's
  untouched frame, never the pass's output) gives the frame's mean luminance; `ExposureMatch::Meter` turns it into an
  exposure multiplier (0.18 / scene luminance, pre-exposure divided out), the same sense as janblade's Automatic
  reading, so his recordings fit unchanged. `UpdatePulse` builds the pass for the meter alone (`MeterWanted()`), and the
  pulse metric's window files nothing then.
- **Recorder.** At most every 6 Ticks, from a sweep the GPU completed, one row: the meter (if under 30 Ticks old)
  beside every held candidate's latest value, keyed by candidate id. 1200 rows kept.
- **Judgement.** Every 50 new rows, on a copy, off the scan's lock: janblade's `MatchEach` -- log-log and log-linear
  fits over lags -5..30 rows; multiplier, divisor or stop value; steady, integer, timer, noisy, the scene's own
  luminance (a divisor at lag 0-1) and edge-lag fits rejected; plateau and frozen rows left out of a fit unless they
  are 40% of it; each half must agree.
- **Offer.** `BestValue` returns the matched candidate only, and 0 (paper white) until a match: no widest-travel
  fallback. A multiplier or divisor in its own units (anchors on the same buffer mean the same number under either
  rule); a stop value as the multiplier it stands for. A match at lag <= 1 is eased in stops (30 frames brighter, 90
  darker): janblade's fix for flashes on camera pans.

Not adopted: janblade's take-over fade from his Automatic level and `AnchorScale`. This fork has no Automatic white
point; the level comes from the user's anchors, and the switch from paper white to anchored happens as it does under
widest travel.

## Known limits

- A judgement costs up to 6.5 ms (host, -O2, 64 smooth columns x 1200 rows) on the render thread every ~6 s.
- A stop value that is never positive is never a `moves` candidate, so the scan's own cadence (barren, quiet) is
  unchanged for it; it is matched only while active.
- D3D12 only (the scan and the pulse pass are). Not offered behind the driver proxy or without the pulse bytecode.

## Evidence

`tests/nr-exposure-match` (pure rule, recordings, wiring guards) and `tests/nr-exposure-scan` cases `match-off`,
`match-on`, `match-eased`, `match-inert`. Needs in-game A/B: see the roadmap note in the report of 2026-10-30.
