# Offline NR-boundary replay and pixel judge

2026-10-20. Contract v1, design written before the Python implementation. The first corpus is
recorded from the production loopback moving scene; a short Cyberpunk capture is a later,
user-assisted step. Phase 1 supplies the offline judge, not a DLL recorder or adapter.

## Scope and integration rules

Replay foundation first, renderer extraction gated by it, layout last. Existing `tests/` and
design-note paths stay put. New fork C++ units use the existing fork-owned vcxproj tail/filters;
there is no Retoque.targets injection and no new edits to other upstream files in this program.
This branch changes no DLL/C++ code. The source schema below is the contract for the parallel
replay seam, not an instruction to copy Dispatch or call the model outside production.

Two modes must have distinct provenance: `nr-boundary` runs recorded pre-NR buffers through
production Dispatch/chain/cadence/composition; future `sdk-pipeline` also runs SR/RR with its
original inputs. V1 judge accepts only `nr-boundary`. After-upscale data cannot prove pre-SR
behavior. A deterministic input/event stream does not make NGX pixels deterministic.

## Files and wire schema (version 1)

UTF-8 JSON, finite numbers only, duplicate keys forbidden. Unknown additional fields are allowed
for forward-compatible diagnostics; all fields listed below are required unless marked optional.
Integers must not be JSON booleans. Plane paths are relative POSIX paths within the manifest's
own directory; no `..`, absolute paths or escaping symlinks. SHA-256 strings are lowercase hex.
The clip identity is SHA-256 of the **exact bytes of capture.json**, not canonicalized JSON.
Finalize manifests atomically only after all files are present and checksummed. Never publish
`complete:true` for a partial recording. Runs can live in different directories and reference
inputs by digest, without copying the input planes.

`capture.json`:
```json
{
  "schema": "retoque-replay-capture", "version": 1, "complete": true,
  "mode": "nr-boundary", "source": "loopback-moving-scene",
  "clock": "recorded-evaluation", "color_space": "linear-rec709", "color_jitter": "removed",
  "depth": {"kind": "device", "inverted": true, "threshold": 0.02},
  "motion": {"direction": "current-to-previous", "units": "pixels",
             "scale": [1.0, 1.0], "includes_jitter": false},
  "regions": {"character": [0, 0, 32, 32]},
  "provenance": {"producer": "commit+dirty-diff-sha", "shader_sha256": "<64 hex>"},
  "frames": [{
    "id": 1, "present": 1, "time_ms": 0.0, "reset": true, "score": false,
    "jitter": [0.0, 0.0], "pre_exposure": 1.0,
    "exposure": null, "exposure_value": 1.0,
    "completion": "fence-complete", "params": {},
    "color": {"path": "f1.color", "encoding": "raw", "dtype": "f16",
              "width": 64, "height": 64, "channels": 4, "row_pitch": 512,
              "sha256": "<64 hex>"},
    "depth": {"path": "f1.depth", "encoding": "raw", "dtype": "f32",
              "width": 64, "height": 64, "channels": 1, "row_pitch": 256,
              "sha256": "<64 hex>"},
    "motion": {"path": "f1.motion", "encoding": "raw", "dtype": "f16",
               "width": 64, "height": 64, "channels": 2, "row_pitch": 256,
               "sha256": "<64 hex>"}
  }]
}
```
The example is structural (one warmup frame cannot be judged). Production captures need a reset
prefix and at least three scored frames/two valid temporal transitions. IDs are consecutive,
presents strictly increase (1 or 4 per evaluation are both valid), times strictly increase.
No implicit missing frame, reset, exposure or jitter defaults. `score` may switch false→true
once, never back. `params` is the typed JSON snapshot of create/evaluate settings consumed at
the boundary; pointers become plane names, and unknown required resources make capture fail.
The seam owns parameter population; the judge preserves params and input hashes, not their SDK
interpretation. Optional `events` can record cuts/FG changes/generation transitions; required
clock/reset/jitter fields remain authoritative. Do not claim an arbitrary mid-session snapshot
restores hidden model history: replay the reset-prefixed warmup identically for every variant.

Supported plane encodings:
- `raw`: little-endian `f16`, `f32`, `u8` (normalized /255); channels 1–4; row_pitch >= width*channels*itemsize;
  exact bytes row_pitch*height, including padding. Native typeless/depth conversion must be explicit.
- `pfm`: PF/Pf, float32, bottom-up, signed scale specifies endian and magnitude; channels 3/1.
  Descriptor width/height/channels must match the file; dtype=`f32`, row_pitch=width*channels*4.
- Color planes have 3/4 channels (alpha ignored for metrics); depth 1, motion 2. Exposure texture,
  when present, is a 1-channel plane; `exposure_value` records the effective scalar, including a
  latched value if no texture is present. Both pre_exposure/exposure_value are positive finite.
- V1 supports full-sized, aligned color/depth/MV planes. Low-resolution/subrect/native packed
  formats must be explicitly normalized by a versioned producer and marked in provenance/params,
  or rejected. Do not silently expand/quantize them in the reader. Color supports
  `linear-rec709` and `srgb-rec709`; decode sRGB before scoring, never use preview PNG pixels.
  Linear planes are pre-exposed: divide input/reference/candidate by the frame's pre_exposure
  for metrics. sRGB planes are display-referred: decode only, without undoing pre-exposure.
  exposure_value remains replay provenance, not an extra grading gain applied by the judge.
  HDR PQ/Rec.2020 and geometry-dependent reprojection are future profiles, not guessed conversions.

Motion samples describe current pixel center → previous pixel center. `pixels` means output
pixels after multiplying by signed `scale`; `uv` additionally multiplies x/y by color width/height.
Required `color_jitter` is `removed` for dejittered boundary color (including the loopback's
DLAA output), or `present` for jittered color. No implicit default: old manifests must be
annotated by their producer and runs rebound to the new capture digest.
When color_jitter=present and includes_jitter=false,
q = p + scaledMV + previousJitter - currentJitter. Otherwise q = p + scaledMV: removed color
never receives a jitter delta, and vectors already including it are not adjusted twice.
Vectors must describe the recorded color grid; the producer normalizes vectors still containing
raster jitter before declaring color_jitter=removed. Jitter remains recorded for NGX even when
removed from color. Forward flow is **not** converted by negation: v1 rejects that direction.
Depth rejection compares current depth with bilinear previous depth at q: absolute difference
<= threshold for `device`, relative difference <= threshold*max(abs(depths),1e-6) for `linear`.
Device depth must be in [0,1], linear depth positive; inverted records the convention but does not
change the difference rule. This is conservative correspondence, not a camera-matrix oracle;
report retained coverage, especially during perspective camera motion. Full-frame color metrics
still include disocclusions that the temporal mask excludes. Optional `motion_oracle` per frame
is a 2-channel plane of expected final backward pixel displacements (including jitter delta);
`motion_tolerance_px` (default 0.05) validates analytic loopback guides. Real games lacking an
oracle do not receive an invented “guides correct” assertion.

`run.json` (one fresh process per variant):
```json
{
  "schema": "retoque-replay-run", "version": 1, "complete": true,
  "capture_sha256": "<capture.json digest>",
  "environment": {"model_sha256": "<64 hex>", "driver": "version",
                  "gpu": "adapter", "runtime": "Proton+vkd3d versions"},
  "producer": {"dll_sha256": "<64 hex>", "commit": "commit+dirty-diff-sha",
               "trial": "fresh-process-uuid"},
  "settings": {"Cadence": 1},
  "requirements": {"min_model": 1, "min_carried": 0},
  "frames": [{"id": 1, "kind": "every", "interval": 1,
              "requested_passes": 1, "completed_passes": 1,
              "completion": "fence-complete", "gpu_ms": 2.5,
              "output": {"path": "f1.output", "encoding": "raw", "dtype": "f16",
                         "width": 64, "height": 64, "channels": 4, "row_pitch": 512,
                         "sha256": "<64 hex>"}}]
}
```
Every capture frame, including warmup, has exactly one run entry in the same order. `kind` is
`every` (interval1), `model` or `carried` (interval2–4), or `skipped` only when the corresponding
capture frame has score=false. A skipped entry requires a nonempty string `skip` reason,
completed_passes=0, interval=0–4 and requested_passes=0–3 (zero means the decision was not
reached). It still requires a fenced output descriptor and finite nonnegative GPU timing;
it contributes no model/carried coverage and breaks temporal/pulse pairs in either run.
A scored skip is INVALID with its reason, never silently omitted. Model/every entries complete all requested
passes (1–3); carried entries complete zero model passes. GPU milliseconds are actual finite
nonnegative per-evaluation measurements, not a fabricated zero on missing timing. Requirements
count scored entries; missing requested coverage/partial chains/incomplete fences → INVALID.
Reference runs must be every-frame (`every`, interval1), except reasoned unscored skips.
The same rule applies to A/A repeats; a carried/model warmup is not an every-frame reference.
Settings may differ by the declared
experiment; candidate DLLs may differ. Model/driver/GPU/runtime and capture digests must match.
Non-reference changes of passes/composition require a matched every-frame reference; the caller
must select that comparison, not mistake an artistic change for quality against a different look.

## Clock, ownership and recording modes

Evaluation ID and logical milliseconds drive cadence, adaptive calm and settling. Present ID is
separate; MFG advancing it by four must not age a probe by four evaluations. The scoped diagnostic
clock delegates to the existing steady clock when inactive. Real GPU fences and timeout deadlines
are never virtualized. Preserve scheduled Reset and delayed certification (2/3/4 evaluations) in
an independent in-flight stress mode; synchronous pixel readback alone cannot validate lifetime.

The recorder copies raw boundary inputs on their owning list, restores states, retains all storage
until proven completion, and only then hands bytes to a bounded writer. No “assumed eight frames”
is accepted. Settled permits recycling discarded storage, not acceptance of pixels. No resource
or feature work when capture is off. Do not drive output readback through the legacy burst flag:
it refuses cadence, changing what the harness is trying to measure. A full readback queue stops
with incomplete status rather than dropping temporal frames. Record scene cuts as resets.

## Judge metrics and verdicts

`python3 -m tools.replay --capture CAPTURE --reference RUN --candidate RUN --out DIR`
accepts optional `--calibration FILE` and `--policy FILE`. Calibration command:
`python3 -m tools.replay calibrate --capture CAPTURE --reference RUN --repeat RUN ... --out FILE`.
Five independent repeated reference runs minimum for a calibrated envelope. The caller supplies
fresh-process runs. Every run requires a nonempty string producer.trial ID; use a fresh UUID
per process, never a variant name reused across launches. Calibration rejects repeated trial IDs
(including the reference ID) as well as identical manifest digests, even if output paths or
diagnostic metadata differ. Byte-identical pixels from distinct trials are valid observations.
Metadata uniqueness cannot prove process independence; that is the runner's duty.
Calibration observations retain each trial ID alongside its run digest and revalidate uniqueness
when consumed. Earlier calibration files without these IDs must be regenerated.

Metrics, for whole-frame and each rectangular region [x,y,width,height]:
- Pulse: e=log2(sum Yout / sum Yin), absolute step in e between consecutive scored frames,
  grouped by destination kind and source→destination kind. Resets/gaps/nonpositive light break
  pairs. Region pulse prevents cancellation of opposing local changes.
- Motion-compensated temporal edit residual: mean absolute change of (out-input), bilinearly
  warped via recorded MV/jitter, with depth/outside rejection; report valid fraction. No coverage
  is inconclusive, not zero error. Scene-cut pairs are excluded.
- Ghosting proxy: current output MAE versus every-frame reference, plus error in rejected
  correspondence/disocclusion pixels; this includes the regions the temporal residual excludes.
  It detects trails relative to a reference, not universal perceptual truth.
- Detail/blur: horizontal/vertical luma-gradient energy; absolute relative deviation from reference,
  with an explicit denominator floor. Report candidate and reference energy to distinguish blur
  from sharpen/noise; more energy is not automatically better.
- Golden: RGB MAE, maximum absolute error, fraction exceeding the policy's fixed pixel tolerance.
  Always score scene-linear floats. NaN/Inf are INVALID; clipping/black output alters error/pulse.
- GPU p50/p95/max, with separate model/carried groups; timing is descriptive, not an FPS verdict.
  A long-tail runtime change cannot be hidden behind a better median.

Summary keys: `golden_mae`, `golden_max`, `golden_bad_fraction`, `ghost_mae`,
`disocclusion_mae`, `detail_error`, `pulse_p95`, `temporal_mae`, `coverage` (mean retained
fraction), plus timing and counts. Pixel tolerance defaults to 0.01 linear units; this is a
reporting bin, not an acceptance threshold. Score every region independently, not only an average.

Policy JSON: `{"version":1,"min_coverage":0.25,"pixel_tolerance":0.01,"floors":{...}}`.
Floors are nonnegative absolute tolerances for the eight error metrics above (all except coverage).
Defaults are zero: **no uncalibrated quality PASS**. Calibration computes per-region upper bounds
as max observed A/A metric plus the corresponding policy floor, binds capture/environment/policy
and reference manifest hashes, stores sample count and individual observations. It is empirical,
not a confidence guarantee. Never interpret the previously measured ~6% variation of mean
cadence residual as “allow 6% pixel error”. Calibrations are immutable inputs, never widened by
the judge; driver/model/corpus/policy changes require explicit recalibration.

Verdicts: INVALID for malformed/mismatched/nonfinite/incomplete inputs; FAIL for exceeding an
applicable calibrated envelope; INCONCLUSIVE for absent calibration, <3 scored frames, <2 valid
temporal pairs or coverage below min_coverage; PASS only for complete, sufficiently covered,
calibrated comparisons within all region bounds. FAIL takes precedence over INCONCLUSIVE for
well-formed data that already proves a regression; INVALID takes precedence over everything.
A bounded envelope detects regression against this reference, not artistic improvement.

Writes `result.json` (version, verdict, reasons, identities, metrics, bounds), `frames.jsonl`
(per-frame/per-region metrics and actual kind), `contact-sheet.png` and `diff-heatmap.png`.
PNG is stdlib-only, fixed-scale display mapping and labeled metadata, not an HDR measurement
container. Contact sheets use first/worst/last scored frames with input/reference/candidate;
heatmap is worst-frame absolute RGB error scaled by the declared pixel tolerance. Raw inputs
remain authoritative. CLI exits 0 PASS, 1 FAIL, 2 INVALID, 3 INCONCLUSIVE. Even errors produce
result.json; stale artifacts from a previous success must not survive an invalid new run.

## Storage, tiers and acceptance

Raw full-resolution color RGBA16F+depth R32F+MV RG16F is 16 B/pixel: 75.6 MiB/frame
at 3440x1440, 22.1 GiB/300 frames; each RGBA16F output adds 11.1 GiB. Stream frames,
retain only adjacent frames/compact summary rows and bounded previews, not the whole corpus.
Use a local cache outside Git; no model/game images in commits or public CI. Quotas and
lossless compression belong to recorder/runner phases. Never silently evict a baseline.

Host: `nr-replay-format` and `nr-replay-metrics` test schema, readers, warp, judge and negative
controls with generated small synthetic data. The manifest gains explicit path/domain/requires
metadata before any directory moves; existing IDs and tiers remain stable. Wine/GPU replay
requires the real model and production DLL later, under the shared nonblocking fixture lease.
Missing required GPU coverage must not appear as PASS; the normal host chain needs no model.
A later required profile combines host tests, Release, moving scene parity, recorder→replay round
trip, A/A controls and focused candidate variants. Phase 1 does not claim that chain has run.

Mutation controls: wrong/truncated/hash-mismatched planes, unsafe paths, nonfinite values,
missing exposure, dropped/duplicate frames, wrong model/runtime, partial passes, incomplete
fences; zero/swapped/scaled MV or wrong jitter against an analytic oracle; induced pulse,
stale output/trails, blur and golden deviations; low correspondence → INCONCLUSIVE. Bright
input exposure transitions with an unchanged relative edit must not create artificial pulse.
