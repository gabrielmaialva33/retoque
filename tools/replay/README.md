# Offline replay judge

Stdlib Python; no dependency installation, GPU, DLL or model required. The judge reads
already recorded inputs/outputs. It does not generate model outputs or capture a game.

From the repository root:

```sh
python3 -m tools.replay --capture /local/clip/capture.json \
  --reference /local/reference/run.json --candidate /local/candidate/run.json \
  --out /local/report
python3 -m tools.replay calibrate --capture /local/clip/capture.json \
  --reference /local/reference/run.json \
  --repeat /local/aa1/run.json --repeat /local/aa2/run.json \
  --repeat /local/aa3/run.json --repeat /local/aa4/run.json \
  --repeat /local/aa5/run.json --out /local/calibration.json
python3 -m tools.replay --capture /local/clip/capture.json \
  --reference /local/reference/run.json --candidate /local/candidate/run.json \
  --calibration /local/calibration.json --out /local/report
```

Exit codes: 0 PASS, 1 FAIL, 2 INVALID, 3 INCONCLUSIVE. Without a compatible A/A calibration,
well-formed input produces descriptive metrics and INCONCLUSIVE, never a quality PASS.
`--policy FILE` must match the policy used for calibration. Do not auto-update a golden or
relax an envelope based on the candidate being evaluated.

Outputs: `result.json`, `frames.jsonl`, `contact-sheet.png`, `diff-heatmap.png`. Contact-sheet
columns are input / every-frame reference / candidate, rows first / worst / last (deduplicated).
PNG Description metadata carries frame IDs, kinds, times and fixed visualization scaling.
Preview pixels are not the measurements; raw scene-linear planes are. A new INVALID run
removes previous image artifacts so stale PASS imagery cannot be mistaken for current evidence.

V1 accepts aligned same-sized planes, raw f16/f32/u8 and PFM, linear or sRGB Rec.709. Other
formats/conventions require an explicit producer conversion or future schema/profile; they are
not guessed. Capture/reset/input provenance and exact model/runtime identities are required.
Process one frame at a time (plus previous-frame history); pure Python still needs realistic
profiling before using long full-resolution corpora. Game frames/models stay local, outside Git.

The complete producer contract, clocks, completion rules and limitations live in
[offline-replay.md](../../docs/dlssnr/design/offline-replay.md).
