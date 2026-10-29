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

## Temporal events and paired cost (`events.py`)

Analyze existing judged metadata without loading raw images, running the model or
requiring NumPy:

```sh
python3 -m tools.replay.events --capture /local/clip/capture.json \
  --runs /local/replay/runs --judge /local/replay/judge \
  --candidate adaptive4 --out /local/analysis/adaptive4
```

The default reference is `every-a` and the independent every-frame control is
`every-b`; `--reference` and `--control` select other names. The control must retain
its calibrated PASS. Candidate, control and reference must have distinct trial
identities and compatible capture, environment, policy and metrics identities.
The capture must contain a complete `provenance.corpus` phase summary. The tool
validates frame coverage, clocks, decisions, completed passes and judge aggregates
against the run manifests before producing a report.

Outputs are `events.json`, derived `frames.jsonl` and `events.md`, in a directory
separate from all inputs. They preserve the candidate's existing numerical verdict
and perceptual `NOT_ASSESSED`. Exit 0 means the metadata analysis completed, even
when the candidate's fidelity verdict is FAIL; exit 2 means invalid input or an
output error. This tool does not revalidate raw pixels or certify rendering.

For each aligned frame and region, pulse comparisons are signed delta
`candidate - control`, paired distance `abs(candidate - control)` and signed
amplitude excess `abs(candidate) - abs(control)`. Equal nonzero pulses have zero
paired distance; amplitude excess can be negative. A reset or missing pulse stays
unavailable with a reason and count, even if a decision transition is present.
History age uses completed model evaluations, including warmup, and preserves the
preceding carried streak when model execution resumes.

Reports group quality and GPU cost by phase, execution kind, history age and event.
Each metric includes mean, p50/p95/p99, extrema with frame identities and missing
counts. These are descriptive frame statistics; small groups are flagged and
overlapping events do not duplicate whole-corpus totals. GPU milliseconds describe
the replay harness bracket, not game FPS, latency or display pacing. `--window`
selects up to 32 preceding/following evaluations around the worst frames (default
4); `--top` selects 1–16 worst frames per region (default 8). Inputs are bounded to
10,000 frames and 32 MiB per file.

For retained qualification evidence, point `--judge` to the retained `artifacts/judge`
directory and add `--qualification /local/evidence/rendering-run.json`. The receipt
checks that the consumed candidate/control `result.json` and `frames.jsonl` are at
the receipt's retained paths and match their hashes and sizes. A moved copy requires
a compatible receipt or analysis without `--qualification`. The result identities
bind the original capture and raw run manifests;
recorded producer identities remain unchanged. Without the receipt, the report
explicitly identifies validation of the current manifests and input hashes only.

The predeclared adaptive2-versus-adaptive4 experiment is in
[`experiments/adaptive2-v1.json`](experiments/adaptive2-v1.json). It fixes three
paired trials, controls, order, quality guards and GPU budgets. Its acceptance
criteria are separate from the existing fidelity judge and do not change defaults.

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

## Metamorphic captures (`transform.py`)

`python3 -m tools.replay.transform` derives a schema-v2 capture that shows the same scene as its source in another
representation (Codex N-03; W3-B01 for the colour order). An exact transformation must replay with the same decisions
and output bytes; a difference is a finding about the seam, the pass or the model, not a quality verdict.

- `pad CAPTURE --out DIR [--depth-base X Y] [--motion-base X Y] [--pad X Y] [--sentinel extreme|nan] [--units K]
  [--mv-low-res keep|on|off]`: depth and motion re-packed at their bases inside larger planes whose margin is a
  sentinel (depth 3e38 or NaN, motion 65504 or NaN in R16G16_FLOAT), the render subrect and bases set; the vectors
  x K and the MV scale / K (K a power of two; refused unless it is exact in the plane's format); MVLowRes toggled only
  where render and display extents agree. Colour is linked unchanged. With `--depth-base 0 0 --motion-base 0 0 --pad
  0 0` it is the identity v2 rendition of a v1 corpus, the reference the others are compared against.
- `colour8 CAPTURE --out DIR --order rgba|bgra|alternate [--gain R G B] [--alpha pattern|source] [--clear-hdr]
  [--swap-every N] [--swap-start I] [--reset-on-swap K ...]`: the float colour quantised once to UNORM8 after a
  per-channel gain (default 8, 3, 0.5, which puts R about 60 steps from B on the loopback corpus), alpha a pattern by
  default; written as R8G8B8A8_UNORM (28), B8G8R8A8_UNORM (87, bytes 0 and 2 swapped) or alternating on the swap
  schedule. `--reset-on-swap K` sets Reset on the K-th scored swap frame in every order, so the all-RGBA reference and
  the alternating capture are one logical sequence. `transform.json` beside the capture lists swap, BGRA and reset
  frames and the measured mean |R - B|.
- `compare RUN_DIR_A RUN_DIR_B`: two `runs/<variant>` directories, decisions (kind, interval, passes, model result,
  skip) and each output read in its own channel order; exit 0 when identical.

`tools/replay/metamorphic_seam.sh <OptiScaler.dll> [corpus capture.json] [out]` derives the captures from a corpus,
replays each through `tests/dlssnr-loopback/run.py --replay-nr` with `variants-metamorphic.json` (off, every-frame
twice, Cadence 2 and 4, adaptive 4, Passes 2 and 3, WorkingScale 0.5, the last running the guide resample over the
padded planes), moves each capture's runs out of `artifacts/`, and compares identity against each geometry
transformation and RGBA8 against BGRA8, variant by variant, plus every-a against every-b as the A/A floor.
`METAMORPHIC_VARIANTS=tools/replay/variants-metamorphic-quick.json` runs five variants instead of nine,
`METAMORPHIC_SETS=colour` only the RGBA8/BGRA8 pair, `METAMORPHIC_DRY=1` only derives and validates. Byte identity is
the right test only while every-a = every-b on that capture (the model was bitwise repeatable on the loopback corpus);
if the A/A line says they differ, judge the pair with a calibrated envelope instead.

**What the seam cannot express yet: a colour format that changes within a capture.** `run_replay.load_capture` refuses
a v2 capture whose colour shape differs between frames ("the shape changes within the capture"); plan v3 carries one
`plane color <format> <source format> ...` line for the whole run; and `replay_nr.h` creates ColorIn and Output once,
in that format, uploads every frame into them and reads the output back from them. A real swap needs, at least: the
validator to accept a per-frame colour format when it is a channel-order twin of the first frame's (same extent, pixel
size and flags); a per-frame colour-format column in the plan (a plan v4); a second ColorIn/Output pair in the twin
format in the harness, chosen per frame for the upload, the parameter block and the readback; the run manifest to
record each output's own format (`compare` already reads them that way); and the in-flight slots to do the same or to
refuse such a plan. `colour8 --order alternate` writes that capture already, so the replay is one command once the seam
takes it. Until then the RGBA8 and BGRA8 sessions are compared whole, which exercises BGRA views, the encode and the
model but not a swap with history kept.
