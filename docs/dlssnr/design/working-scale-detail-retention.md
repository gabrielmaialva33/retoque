# How much of NR's change survives a smaller working scale

Measured 2026-10-29 on a replayed game clip. Nothing in the code changes with this note; it gives the image side of a
choice that, until now, was made on cost alone (`working-scale-auto.md`, `model-cost-vs-working-scale.md`).

## Why

`WorkingScale=auto` is 0.5 on Turing and Ampere and `RRWorkingScale` defaults to 0.5 on every card, both chosen by what
the model costs. RTX40MFG-Unlock (`source/native/dlss5_auto.h:27-30`, MIT, their issue #692) measured the other side on
their pipeline at 1080p: at 75% they kept 61.5% of DLSS 5's change and 22.9% of its fine detail, at 50% only 41.1% and
8.5%, and their automatic scale never goes below 75%. Their enlargement is not ours, so their numbers do not transfer;
this note measures ours.

## How

- Clip: Cyberpunk 2077, `dlssnr-replay/20261020T234326Z` recorded by the replay recorder (`offline-replay-recorder.md`),
  33 frames at 3440x1440, after Ray Reconstruction (`ray_reconstruction: true`), finalized as a copy.
- Replay: `tests/dlssnr-loopback/run.py --replay-nr` through the production seam, build `c862bd2e`, RTX 4090, driver
  615.78.08, GE-Proton11-11, model 310.9.16, with `LOOPBACK_REPLAY_VARIANTS` off / `RRWorkingScale` 1, 0.75 and 0.5
  (`Cadence=1`). 17 of the 33 frames ran the model in all three; the first 16 were the working resolution settling.
- **The clip is at the RR placement, so `WorkingScale` does not apply to it**: a first run varying `WorkingScale` gave
  three byte-identical outputs (all at `RRWorkingScale`'s 0.5). The non-RR route is not measured here.
- Metric (`tools/replay/experiments/working-scale-retention.py`): the edit is the NR output minus the game's frame;
  "kept" is the projection of the edit at scale s on the full-scale edit, `<e_s, e_1> / <e_1, e_1>`, and "corr" their
  correlation. "All" is the whole edit; "fine" is the edit minus a Gaussian blur (sigma 2 px), the detail the model adds.
  Computed on log2 luminance (where the composition works) and on linear RGB. Medians over the 17 frames; the p10 is
  within 0.04 of every median. Raw numbers: `~/.local/state/dlss5/evidence/working-scale-retention-20261029.json`.

## Result

| | 0.75 | 0.50 |
|---|---:|---:|
| Whole change, log2 luminance, kept | 0.98 | 0.88 |
| Whole change, log2 luminance, corr | 0.91 | 0.62 |
| Fine detail, log2 luminance, kept | 0.70 | 0.34 |
| Fine detail, log2 luminance, corr | 0.68 | 0.30 |
| Fine detail, linear RGB, kept | 1.11 (energy 1.28) | 0.75 |
| Model GPU time, RTX 4090, 3440x1440 (median) | 5.60 ms | 3.75 ms (1.00: 7.49 ms) |

- The overall look survives scale well in our pipeline: 88% of the full-scale change at 0.5, against 41% in theirs.
  The matched-residual enlargement (`Transfer=1`) is the likely reason; it was built to carry the edit across scale.
- **Fine luminance detail does not**: a third of it is left at 0.5, and its shape agrees poorly with full scale (0.30).
  At 0.75 it is 70%, at about +1.9 ms over 0.5 on a 4090.
- In linear RGB the fine band at 0.75 has *more* energy than at full scale (1.28): the enlarged edit is not a weaker
  copy of the full one but a different one in the fine band. A visual check is needed before reading that as detail.

## What it means for the defaults (not decided here)

- **RTX 40 and later, RR placement** (`RRWorkingScale`, 0.5 on every card): 0.75 keeps twice the fine detail of 0.5 for
  ~1.9 ms on a 4090 at 3440x1440. That is the strongest candidate for a change, but it is one clip and one scene.
- **RTX 20/30** (`WorkingScale=auto` 0.5): the 3060 already pays ~18 ms at 0.5 at 1440p (`working-scale-auto.md`); 0.75
  would roughly double it. The default stays on cost; what this note adds is that the user pays it in fine detail, which
  the menu could say.
- Before any default moves: more clips (non-RR route, a slow and a fast camera), the A/A floor with
  `LOOPBACK_REPLAY_CALIBRATE`, and a visual check of the 0.75 fine band.
- **Pin the RR provider and preset (added 2026-10-30).** NR's input at the RR placement is Ray Reconstruction's
  output, so the RR model is part of what is measured. NVIDIA DLSS SDK 310.9.1 (release 2026-09-08) added the RR
  transformer mode as Preset F. Its `nvsdk_ngx_defs_dlssd.h` moved the "Default model" comment from Preset D (310.7.0)
  to Preset F ("Default model RR2"). A game or OTA provider update can therefore change the RR model without any
  setting changing. This note does not record the RR provider build or preset of its clip. Every further clip should
  record both, and clips compared across scales should use the same pinned provider and preset.
