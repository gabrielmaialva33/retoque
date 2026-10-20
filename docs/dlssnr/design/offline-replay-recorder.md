# Offline replay: recording a game's NR-boundary frames

2026-10-20. Design note written before the code (DEVELOPMENT.md, design-doc-first); section "As built" at the end
records where the code differs. This is the producer of real-game corpora for [offline-replay.md](offline-replay.md) (format, judge) and
[offline-replay-seam.md](offline-replay-seam.md) (replay through the production pass). Status: implemented on D3D12 after-upscale; verified on the loopback moving scene, not yet run in a game.

## Why

The replay harness judges the production pass on recorded frames, but its only corpus is the loopback moving
scene: a lit cube on a dark background. It is enough to gate refactors bit for bit; it says nothing about what a
player sees, for example light pulses on a night street, carried edits smearing hair, or MinRatio on neon. Those
need the frames a game actually hands the pass. A screenshot or a video cannot be replayed: the model needs depth,
motion vectors, exposure and the parameter block, at their native precision.

## What a recording is

A recording is a short clip, seconds rather than minutes, taken by one menu press. It holds exactly what
`EvaluateAfterUpscale` receives, captured before the pass writes anything:

- **Planes**, copied raw in their native DXGI format and extent, each made typed for the copy the way
  `capture::FrameCapture` does:
  - the Output texture: the upscaler's colour, the pass's input, read and written in place;
  - Depth;
  - MotionVectors.

  The guides stay at render resolution when the game supplies them there, which is Cyberpunk's case (3440x1440
  colour with guides at about 2293x960). They are not resampled: resampling would bake one interpretation of the
  guides into the corpus.
- **Per frame:**
  - the present counter;
  - steady-clock milliseconds since the first recorded frame;
  - whether the evaluate came through the Ray Reconstruction placement;
  - the parameter block as `GatherFrame` reads it: create flags, Reset, jitter, MV scale, render subrect, depth
    and motion subrect bases, DLSS.Pre.Exposure, and whether an ExposureTexture was bound.
- **Provenance:**
  - the DLL build;
  - the model and loader identities (`NgxInfo`);
  - the effective `[DlssNr]` settings as the timing contract prints them;
  - the GPU and driver;
  - the game executable's name.

The first recorded frame is marked `reset: true` in the manifest whatever the game said. A replay starts from an
empty model history, and the replay compares replays with each other, never with the live frame.

## Trigger and lifetime

- **Inert when off:**
  - one relaxed atomic load per evaluation;
  - no allocation, no copy, no readback, no thread;
  - no new configuration key (the pair capture's rule, comparison-capture.md section 1).
- **Start:**
  - a menu button beside the pair capture, "Record replay clip", or the trigger file `dlssnr-replay.trigger`
    beside the DLL, polled with the existing trigger check;
  - it refuses with a reason when the route is not after-upscale D3D12, or when an ExposureTexture is bound (v1
    does not transport one);
  - it refuses when the drive holding the output folder has less free space than the clip's estimate x 1.25.
- **Length:** 6 seconds of steady-clock time, capped at 360 frames. The replay needs about 1.4 s of warmup
  before its first scored frame (two 500 ms settle gates under tracked recording, then the model's creation),
  so a 6 s clip leaves about 4.5 s judged.

## Copy and certified readback

The copies are recorded on the game's command list, inside `EvaluateAfterUpscale`, before `Dispatch`, with the
same barrier discipline as the pair capture: arrival state, then COPY_SOURCE, then arrival state, with the guide
rest states from `GuideRestState`.

- **Readback slots:** a ring of K = 6 slots, each holding three readback buffers sized by
  `GetCopyableFootprints`.
- **Tracking:** each slot has its own `Submission::Usage`, tracked on the list that carries its copies.
- **Freeing a slot:** only on a reusable certificate or a settled usage, the rule the adaptive probes use.
- **Data:** read only from an accepted certificate (Reset, one execution, completed fence).
- **No CPU wait** on the render thread, ever.

A frame whose copies cannot be certified ends the recording. So does a frame that finds no free slot. The clip is
then the contiguous run of certified frames before it, finalized with `complete: true` for that prefix and the
reason it stopped in provenance. Temporal frames are never dropped in the middle.

## Writing

A writer thread, the pair capture's pattern, takes certified frames in order:

1. writes `f<id>.color`, `.depth` and `.motion` (raw, tight rows) into `dlssnr-replay/<UTC stamp>/`;
2. hashes each plane;
3. at the end, writes `capture.json` through a temporary name and an atomic rename.

A crash leaves a folder without `capture.json`, which no reader accepts. Recordings are never deleted
automatically, and the folder is local: game frames never enter the repository.

## Size, at 3440x1440 with guides at 2293x960 and RGBA16F colour

| | Per frame | 6 s clip |
|---|---|---|
| Data | about 57 MB | 1.2 GB at 21 fps, 3.4 GB at 60 fps |
| Readback ring (6 slots) | about 340 MB of host memory, while recording only | |

The writer must sustain about 1.2-3.4 GB/s. If it falls behind, the ring fills and the recording ends early,
honestly, as above.

## The format changes this needs (capture schema v2)

v1 (offline-replay.md) requires equal-sized planes in fixed dtypes. A game recording needs three extensions:

1. **Per-plane extent and DXGI format,** with the dtype derived from the format. The seam uploads planes in their
   recorded format and extent, so the pass reads the same bytes the game gave it.
2. **The guide geometry per frame:**
   - render subrect width and height;
   - depth and motion subrect bases;
   - MV scale;
   - the create flags (MVLowRes, MVJittered, DepthInverted, IsHDR, AutoExposure).

   The harness writes these into the parameter block as the game did.
3. **For the judge:**
   - warps map output pixels to guide pixels through the recorded subrect and scale, the way the pass does;
   - colour decoding covers the formats a game's NR boundary uses (RGBA16F, R11G11B10F, RGB10A2, RGBA8 sRGB).

Owners:
- **Recorder (DLL) and seam/harness:** Claude. Recorder in a fork-owned unit, `DlssNr_ReplayRecorder*`, with one
  call in `EvaluateAfterUpscale`. Seam plan v3 with per-plane formats and geometry.
- **Judge v2 reader and warps:** Codex.

## Validation

- **Host:** the recorder's state machine (start refusals, ring backpressure, certificate rules, incomplete-prefix
  finalize, manifest atomicity) against fakes; schema v2 readers and mutations.
- **Loopback, the first producer test with no game:**
  - record the moving scene through the production DLL with the recorder on;
  - replay that capture through the seam;
  - the replay of the recorded clip must equal the replay of the same scene's synthetic corpus wherever the two
    describe the same frames.
- **Default off:** the 19-variant synchronous gate and the four in-flight gates stay identical (the recorder is
  never triggered there).
- **Game:** one short Cyberpunk session by the user (several clips: city walk, driving, night neon, close-up),
  then local replays only.

## As built (2026-10-20)

- **Trigger:**
  - the menu button "Record replay clip", under Compare beside the pair capture;
  - or the existing trigger file `dlssnr-capture.trigger` beside the DLL containing the word `replay` (`burst` and
    anything else keep their meanings). No new file name.
  - NR must be enabled: the hook sits after `EvaluateAfterUpscale`'s own early returns.
- **Writer:**
  - it writes straight from the mapped readback, without hashing and without an intermediate copy;
  - a first version that hashed each plane on the writer filled the ring after 14 frames on the loopback, and at
    3440x1440 (about 57 MB a frame) the hash alone would cost about 190 ms a frame;
  - a slot is free again only when the writer is done with it, so the ring bounds disk speed as well as GPU
    completion.
- **The manifest:**
  - every `sha256` is null and the capture says `"finalized": false`;
  - `python3 -m tools.replay.finalize <capture.json>` checks every plane's byte count, hashes it and rewrites the
    manifest atomically with `"finalized": true`;
  - a finalized capture is refused, so its identity never changes once replays reference it.
- **Shape:** a clip keeps the shape it started with. A change of size or format mid-clip ends it at the frames
  before.
- **Loopback verification** (`LOOPBACK_REPLAY_RECORD_CHECK=1`, `tests/dlssnr-loopback/run_replay.py`):
  - the moving scene ran 640 frames with NR on and the trigger file asking for a clip;
  - the recorder wrote 360 frames, stopping at its length;
  - finalize hashed them;
  - every recorded depth and motion plane was byte-identical to the scene's own for the same frame (360 of 360);
  - every recorded colour plane that overlaps the NR-off corpus was byte-identical to it (62 of 62).
- **Not yet:**
  - replaying a v2 capture: the seam's plan and the judge's reader still take v1 (equal extents, fixed dtypes);
  - a game recording;
  - native Windows.
