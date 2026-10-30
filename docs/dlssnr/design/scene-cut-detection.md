# Scene-cut detection: dropping the model's history when the game does not say it cut

Status 2026-10-30: **phase 1, the rule only.** A pure, host-tested detector (`retoque/dlssnr/DlssNr_SceneCut.h`,
`tests/nr-scene-cut`). **Nothing in the renderer calls it**: no ini key, no GPU pass, no shader change, no change to
any route. Its thresholds are calibrated on synthetic grids only. Phase 2, below, proposes the feed, the key and the
wiring; none of it is implementation authorization.

## Why

The model drops its history only when `DlssNrFrameInfo::Reset` is set (`shaders/dlssnr/DlssNr_Common.h:112`). Today
that flag comes from:

- the game, through `NVSDK_NGX_Parameter_Reset`, on routes that see the game's upscaler parameters
  (`DlssNr_Dx12_Inputs.cpp:124`);
- our zero-guide policy, `ZeroGuideReset`, on zero guides (`DlssNr_Dx12.cpp:4565-4566`) and on dead motion vectors
  ([zero-motion-detection.md](zero-motion-detection.md)), marked `ResetIsPolicy` because nothing cut;
- synthesized motion's FidelityFX scene-change passes, when the pass estimates its own motion (`DlssNr_Dx12.cpp:5244`).

So a cut is missed wherever the game's flag does not reach the pass or the game never sets it: every present-time
route with the game's own guides (the D3D11 bridge's host replaces the captured frame's `Reset` with its own,
`DlssNr_PresentHost.cpp:719`), and any title that does not flag its cutscene cuts. A missed cut warps one picture's
history into the next for a few frames. [synthesized-motion.md](synthesized-motion.md) lists cuts as an open risk
(risk 2).

## Is there a per-frame statistic already? No, not on the default path

Surveyed 2026-10-30 on `2d36321b`:

| Signal | Where | Runs when | What it holds | For cuts |
|---|---|---|---|---|
| Pulse grid | `precompile/dlssnr_pulse.hlsl`, collected in `UpdatePulse` (`DlssNr_Dx12_Diagnostics.cpp:390`) | `[DlssNr] PulseMetric` only (default off), D3D12 | 16x16 tiles: luminance of the output and of the game's frame, pixels read; read back through a 16-slot ring, no wait | exactly the statistic this rule reads, but opt-in |
| Adaptive cadence probe | `DlssNr_CadenceAdaptive.h:12-24` | `CadenceAdaptive` and `Cadence` > 1 only, sparse | 16x16 motion and carry rejection | opt-in, sparse, measures our carried edit |
| Motion probe | `DlssNr_MotionProbe.h:31-34` | `ZeroGuideReset` only, every 8th evaluate | 32x32 depth, slope, zero-motion counts | 8 frames apart: too sparse for a cut |
| Exposure meter | `DlssNr_Dx12_Diagnostics.cpp:705-858` | the game supplies exposure | tile 0 only: the game's exposure scalar | one number; most engines' eye adaptation smooths across a cut |
| FidelityFX scene change | `SynthMotion_Dx12`/`_Vk` | synthesized motion only | histogram divergence | already a reset source, about 3 frames late ([measured](synthesized-motion.md)) |
| Detail measure | `detail-measure.md` | a 60-frame still-scene session | | not continuous |

The pulse grid is the right statistic, but it is opt-in. Running any of these by default, even only to log, adds
GPU work to NR's default path, which [DEVELOPMENT.md](DEVELOPMENT.md) rule 1 forbids without a key. So phase 1
stops at the rule. A shadow mode that runs by default would need its own decision, and the cost measured first.

## The rule (`DlssNr_SceneCut.h`)

Input: per-tile mean luminance of **the game's frame** on a 16x16 grid. `FromPulseGrid` reads the pulse grid's
channel 1 over channel 2 and never channel 0, our output: a statistic of what this pass writes would be a feedback
loop. Any unit works: only ratios and order are used.

Per frame (`Frame`): log2 luminance with a floor ten stops below the frame's mean, its median and spread, and each
tile's **rank** (0..1, ties sharing their mean). A frame with no picture is *blank* and is never compared: black,
flat (spread under 0.05 stops), or noise (neighbouring ranks differ by more than 0.22 on average; unrelated ranks
differ by a third, and a picture is smooth at this scale).

Per consecutive pair (`Compare`), four numbers:

1. **Incoherence**: the mean change of rank against the previous frame moved by the best global shift, searched up to
   two tiles (an eighth of the frame) in half-tile steps, bilinear. Ranks do not move under any change of light that
   keeps order, such as exposure, a white flash, a fade or a tone curve, and the shift lines up a pan. This is the
   motion-coherence term: low when the new frame is the old one moved.
2. **Breadth**: the share of overlapping tiles whose rank moved more than 0.25. A cut changes most tiles. A burst
   changes the ones it lights.
3. **Distribution**: 1-Wasserstein distance, in stops, between the two frames' median-centred distributions of
   log luminance. It is motion-invariant and says that the light really changed shape.
4. **Polarity**: each tile's change in stops less the frame's median change, weighted by how far it went past
   0.5 stops. Polarity is the brightening share of that weight. A muzzle flash, an explosion or a lamp moves light
   one way, while a new picture moves it both ways. It is weighted by size rather than counted, so a burst's few stops
   outweigh the grain and misregistration around it. With a count, muzzle-flash onsets read 0.85-0.88; weighted,
   they read 1.00.

A pair is a cut when incoherence is at least 0.10, breadth at least 0.06, distribution at least 0.05 stops, and
polarity is within [0.05, 0.95].

Hysteresis (`Detector`):

- Frames pair only when their present sequence numbers are consecutive and less than 250 ms apart. This is the
  pulse metric's own pairing, so a frame the pass could not measure breaks the pair.
- A readback older than one already fed is ignored.
- The first 3 comparisons after a start or `Reset()` are not believed (`Warmup`).
- No cut is believed within 30 frames of the last believed cut or of a reset the game asked for (`Cooldown`). A
  game's own reset is reported as `GameReset` whatever the scores say.
- Counters cover believed cuts, warm-up and cooldown suppressions, game resets and blank pairs, so a session can be
  read from the log.

Request (`Pending`): a cut is found on a readback that arrives frames after the cut, so it is armed and taken once, at
the first later frame that reads the reset. That frame gets one of five answers:

- `Reset`: the key is on.
- `Shadow`: the key is off; the cut is only logged.
- `Late`: more than 4 frames after the cut. By then the model has rebuilt its history on the new scene, and a reset
  would cost a stability dip and fix nothing.
- `GameAlready`: the game reset on that frame itself.
- `None`: nothing is armed yet.

### Prior art, for ideas only

- **FidelityFX optical flow's scene-change detection** (in tree, MIT): it compares luminance histograms. It is
  motion-invariant and flash-sensitive.
- **DLSS5VKLayer `helper/main.cpp`** (AGPL; read, not copied): mean absolute grey difference over two frames. Pans
  and flashes defeat that rule, and those are the cases the shift and polarity terms address.
- **DLSS5-NeuralScreen `scene_cut.py`** (PolyForm Strict; read, not copied): contrast normalisation plus a
  best-translation search. Its docstring records the same lesson: one absolute threshold fails both ways.

The rank, polarity and hysteresis terms here are our own.

## Synthetic evidence (host only)

`python3 tests/nr-scene-cut/run.py` on 2026-10-30 (g++, ASan/UBSan). A world is a smooth field of light with a sky
every world shares, the hard case of two scenes with one broad layout. A frame is its 16x16 tile means with 1% grain
(8% in one case). The table gives the maximum score across each sequence's compared pairs. "Both ways" is the
maximum incoherence and breadth of a pair whose polarity was inside the cut band.

| Sequence | Cuts | Max incoherence | Max breadth | Both ways |
|---|---:|---:|---:|---|
| static, 1% / 8% grain | 0 / 0 | 0.005 / 0.031 | 0 | same |
| pans 0.1, 0.5, 1.5 tiles/frame, diagonal, tilt | 0 | <= 0.042 | 0 | same |
| walk forward (2% zoom/frame), and with a pan | 0 | 0.011 / 0.044 | 0 | same |
| flash, gain x6 / added light | 0 | 0.005 / 0.047 | 0 | same |
| explosion over a fifth of the frame, shaking camera | 0 | 0.085 | 0.066 | 0.036 / 0 |
| muzzle flash, a quarter of the frame, 4 stops | 0 | 0.175 | 0.209 | 0.005 / 0 |
| fade to black, black, fade in another scene | 0 | 0.072 | 0.025 | 0.072 / 0.025 |
| cross-dissolve over 24 frames; eye adaptation 4 stops | 0 | 0.032 / 0.006 | <= 0.008 | |
| hard cut | 1, at the cut frame | 0.258 | 0.383 | |
| 100 world pairs, panning across the cut | 95 found, 0 extra | | | |

- **The misses are cuts into a picture that is one large bright or dark area.** Those read as light switched on,
  which is the polarity term's trade. In a 400-pair sweep the band [0.05, 0.95] kept 382 of 400. A [0.15, 0.85]
  band kept 357.
- **The tightest margin is the fade**, at 0.072 against 0.10, from the frames just out of black.
- **Mutations are caught.** Without polarity (1 check fails), without the shift search (4), without the noise blank
  (2) and with a 1-frame cooldown (3).
- **Not covered:** real content (motion blur, particles, foliage, film grain, HUD, letterboxing), TAA jitter on the
  grid, and the GPU half. HUD tiles are identical across a cut and lower its scores. At 16x16 they are a small share,
  but a HUD-heavy title is a real test.

## Latency

A readback completes after the GPU has run the frame (`Submission::Completed`, no CPU wait), normally 1-3 frames
later. FidelityFX's scene change was measured 3 frames late. The cut frame itself is always evaluated with the old
history; the reset lands L >= 1 frames later. Frames t .. t+L-1 have already blended the old scene, and the reset then
throws away L frames of good history on the new one. Whether that trade helps is the main A/B question. Acting on frame
t itself would need the verdict before the model is recorded, which means a CPU wait on the GPU. That is rejected.

## Phase 2 proposal (not implemented)

### Step A: feed it from the pulse readback, no new GPU work

- Expose a completed slot's grid from `DlssNr_Pulse_Dx12`. Today only `Light(slot)` is public
  (`DlssNr_Pulse_Dx12.h:48`).
- In `UpdatePulse`'s collection loop (`DlssNr_Dx12_Diagnostics.cpp:435`), for each completed, non-stale slot, call
  `detector.Add(slot.seq, slot.ms, FromPulseGrid(grid), slot.gameReset)`, and `pending.Arm(seq)` on `Outcome::Cut`.
  `PulseSession::Slot` (`DlssNr_Dx12_Internal.h:502`) gains the frame's non-policy reset bit.
- Call `detector.Reset()` and `pending.Clear()` where the pulse shape changes (`DlssNr_Dx12_Diagnostics.cpp:446`)
  and in `StopPulseLocked` (`:369`).
- Take the request where the frame's reset is read, beside the dead-motion rule (`DlssNr_Dx12.cpp:1100-1108`):
  `pending.Take(g_pulse.entered, key, frame.Reset && !frame.ResetIsPolicy)`. OR `Request::Reset` into `frameReset`,
  and log each outcome with `LogLine`, rate-limited (`DlssNr_LogRate.h`).
- `UpdatePulse` runs last in Dispatch (`DlssNr_Dx12.cpp:3432`), after the reset was read, so a cut armed there is
  taken one Dispatch later. `kMaxLateFrames` already counts that frame.
- Treat a detected cut as policy for the output stabilizer at first (`stabilizerReset`, `DlssNr_Dx12.cpp:2723`). A
  false positive would otherwise reset the stabilizer as well. Promote it to a real cut once A/B shows the precision.
- Add one new key, `[DlssNr] SceneCutReset`, default false. Rule 4 round trip: `Config.h` declaration,
  `Config.cpp` read and save, shipped `OptiScaler.ini`.
- Shadow mode is `PulseMetric` on with `SceneCutReset` off. It always logs and counts.

### Step B: the cheapest pass of its own

Step B is needed only if the key must work without `PulseMetric`. A key that silently does nothing is rule 4's
failure in another form, so step A alone must document that dependency.

- **The pass:** a luma-only sibling of `dlssnr_pulse.hlsl` that reads only the game's frame (the encode's untouched
  copy). One dispatch of 16x16 groups of 8x8 threads reads every second pixel each way: a quarter of one frame,
  half the pulse pass's loads. It writes 256 (sum, pixels) texels to a 2 KB readback per slot.
- **Placement:** it is built only while `SceneCutReset` is on. It is shared with the pulse pass when that is on,
  since that pass already computes the same sums. The HLSL lives in its own file, so `dlssnr.hlsl` and its two
  bytecode headers stay as they are. DXIL only, with optional bytecode via `__has_include`. There is no Vulkan pass
  to share with.
- **Estimated cost:** 4K RGBA16F is 8.3 Mpx, so a quarter of it at 8 B a pixel is about 17 MB read. That is about
  0.02 ms bandwidth-bound on a 1 TB/s card, plus a 2 KB copy. For comparison, the whole FidelityFX estimator measured
  0.32 ms at 1080p. This is an estimate: measure it with the [GPU timing](gpu-timing.md) contract before quoting it.

## A/B evidence needed

1. **Shadow logs against ground truth.** On a route where the game sends its own reset, log the scores at every
   `GameReset`. The game's flag is then a free label: detections near game resets measure recall, and detections far
   from any reset are false-positive candidates to check by eye.
2. **Content classes.** Cutscene cuts, whip pans, explosions and muzzle flashes, menus and pause overlays, fades,
   loading screens, photo mode, HUD-heavy and letterboxed titles. Record each with its scores.
3. **Readback lag per route.** The distribution of taken-minus-cut frames, against `kMaxLateFrames`.
4. **Image A/B across cuts with the key on and off.** Use pair capture or offline replay of the 1-5 frames after a
   cut. D3D12 behaviour qualifies only on native Windows; Proton accepting the calls is not an oracle.
5. **The cost of a false reset.** Force resets mid-scene and measure the dip with the detail measure or pulse metric.
6. **Step B's GPU time,** if step B is taken.
