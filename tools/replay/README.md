# Offline replay judge

Stdlib Python works without dependency installation, GPU, DLL or model. The judge reads
already recorded inputs/outputs. It does not generate model outputs or capture a game.

From the repository root:

For large captures, use the optional, automatically selected NumPy backend in a uv-managed
environment: `uv run --with numpy python -m tools.replay ...`. Never use system pip.
`RETOQUE_REPLAY_BACKEND=python` forces the scalar reference; `numpy` requires acceleration
and `auto` (default) falls back cleanly if unavailable. Result metadata names the backend.
Both paths validate all warmup inputs but calculate metrics only for scored frames, retaining
the immediate predecessor for the first pair. Calibration files from before `metrics_revision:2`
must be regenerated (first-pair and unpaired-disocclusion semantics changed).

CPU metric-only benchmark (no model, GPU or files read):
`uv run --with numpy python -m tools.replay.benchmark --width 640 --height 360 --repeats 3`.
It reports paired/unpaired medians and labeled pixel-count extrapolations, not whole-judge timings.

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

V1 accepts aligned same-sized planes, raw f16/f32/u8 and PFM, linear or sRGB Rec.709.
V2 accepts finalized native-DXGI recorder captures and matching v2 runs, including
render-sized guides, recorded subrectangles/bases and per-frame motion scale. Run
`python3 -m tools.replay.finalize capture.json` on an unfinished recorder clip first.
The strict whitelist, typed-component and colour rules are documented in the schema.
Optional `source_format`/`source_flags` describe provenance only. Unknown formats
or out-of-bounds geometry are INVALID; conventions are never guessed. Capture/reset/input provenance and exact model/runtime identities are required.
Captures explicitly declare `color_jitter: "removed"|"present"`; DLAA boundary output uses
`removed`. Runs require a fresh-process `producer.trial` string. Reasoned `kind: "skipped"`
entries are accepted only on unscored warmup frames and break temporal pairs; scored skips
remain INVALID. A/A rejects reused trial IDs even with different paths or manifest hashes.
Process one frame at a time (plus previous-frame history); pure Python still needs realistic
profiling for complete decoding/I/O/preview cost on long full-resolution corpora.
Game frames/models stay local, outside Git.

The complete producer contract, clocks, completion rules and limitations live in
[offline-replay.md](../../docs/dlssnr/design/offline-replay.md).

## Short game clips: a held warmup

The replay scores only frames after the pass's start-up gates, so a recorded clip shorter than its 1.5 s warmup has
nothing to score. A game can force that on the recorder by outrunning its writer (the first Cyberpunk 2077 clips
ran 0.4 to 1.2 s). `hold` writes a new finalized capture that opens on the clip's first frame held still, with zero
motion, for 1.6 s of logical time at the clip's own frame interval. The clip follows, its first frame reset, and
every one of its frames is scored. The planes are linked, not copied, and the source capture is left as it was.

```sh
python3 -m tools.replay.finalize /local/clip/capture.json
python3 -m tools.replay.hold /local/clip/capture.json /local/clip-held [--ms 1600]
```

The held frames reach the gates; they are never judged. The clip's first frame is reset, so the model's history
starts on the game's frames, as at the start of any recording, and a cadence variant's first frames after it are
model frames. Those first few scored frames are therefore alike across variants by construction.
