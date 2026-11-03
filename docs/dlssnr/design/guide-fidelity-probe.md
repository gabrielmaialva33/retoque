# Guide fidelity probe: do the game's guides explain the picture NR is shown? (designed, not built)

Date: 2026-11-03. Status: **designed, reviewed, parked**. Nothing is implemented. Source: the 2026-11-01
present-route audit (opportunity 2, gap 3).

## Question

On the present route the model is shown the backbuffer, after post-processing, HUD, grain, lens effects and any
scaling after the upscaler. Its guides are the upscaler's (the ring capture). The identity rules
([present-real-guides.md](present-real-guides.md)) prove the capture belongs to the frame and has the right
extent. They say nothing about whether the motion still describes how this picture moved, and nothing measures it.

## The proposed measure

For sparse taps `p` (a 32x32 grid of cells, 4x4 taps each) at the model's working size:

- `L(p)` is this frame's proxy luma (the UI mask's luma);
- `K` is the previous frame's;
- `mv` is this frame's motion in working pixels, read as the cadence reads it. The previous position is
  `p + mv` (`dlssnr_cadence_rule.h`, `CR_MOTION`).

From those:

- `e0 = |L(p) - K(p)|`;
- `e1 = |L(p) - K(p + mv)|`, sampled bilinearly at pixel centres;
- a tap "tells" when `e0` is above a threshold and `p + mv` is inside the frame.

Reported per window:

- `explained = 1 - sum(e1)/sum(e0)`, which may be negative and is not clamped;
- the share of taps improved;
- the share that changed where the guides say nothing moved;
- the coverage counts.

The after-upscale route is the empirical baseline, not ground truth. This is a sampled luma residual, not an
image-quality score: disocclusion, lighting, blur, depth of field, jitter, noise and HDR clipping of the encoded
proxy all leave `e1 > 0` with correct guides.

Two passes: a "keep" that writes `K` on evaluate `8k - 1`, and a "score" on evaluate `8k`. The insertion point is
after `cadenceFrame` is filled in `DlssNr_Dx12::Dispatch`. An independent review confirmed that `modelInput` and
the motion are readable (NON_PIXEL_SHADER_RESOURCE) there on every route that reaches it. It also confirmed the
sign and scale conventions. A `[DlssNr] GuideFidelity` key, off by default, D3D12 only.

## Why it is parked

The review (2026-11-03) found contracts the design did not close:

1. **GPU order keep -> score (P1).** The two passes go on different game command lists. `Completed(score)` does
   not prove the keep executed, or executed first. A keep on a list the game drops leaves stale `K` read as
   valid. Lists on unordered queues can race on `K`. Each pair needs producer and reader evidence, refused
   without any wait when the order is not proven. The next keep must not overwrite `K` while a score reads it.
2. **Lifetime covers the keep (P1).** The keep has no readback, so "no readback pending" is not "no command uses
   the probe". One usage/lease must cover keep, score and copy. That includes constant buffers, descriptor heaps
   and `K`, through replay and retirement, on off, resize, device change and shutdown. Imitating the motion
   probe's readback ring is not enough.
3. **Frame continuity (P2).** "The previous evaluate that reached the probe" is not "the previous frame". Early
   returns in Dispatch can skip a frame between keep and score. A same-size stream switch is not seen. Packing,
   transfer, motion-scale or generation changes alter `K`'s meaning without a resize. The pair needs a
   per-entry counter taken before the early returns, and a stream identity from the caller.
4. **Which motion is measured (P2).** `cadenceFrame.mvToWork` is the corrected scale. With `RenderMotionScale`
   off the model gets the legacy scale. After carried frames it gets the chain. v1 would measure "the game's
   current motion between two frames", and must say so, or label the legacy mode separately.
5. **Resets and holds (P2).** A policy reset (`ZeroGuideReset` on a dead field) is not a cut. Excluding every
   reset hides exactly the dead-field case. `HoldFrame` must be excluded or measured separately.
6. **Cost (P3).** The keep reads the full proxy: about 49.5 MB per probe at 3440x1440 RGBA16F, plus the R16 write,
   concentrated on two consecutive frames of each eight.

Closing these is a larger job than the value of a log-only diagnostic justified on 2026-11-03. The Streamline tag
inventory ([sl-tag-inventory.md](sl-tag-inventory.md)) answers the more immediate question of the same audit
item: whether a HUD-aware present can use the game's own HUD-less or UI-alpha buffer.

## If it is picked up

- Rewrite the design with the six points closed before code.
- Tests the review asked for, beyond the rule cases:
  - a cancelled keep with a completed score;
  - late or inverted submission, and a second queue;
  - off, resize or shutdown between keep and score;
  - a consumed readback that still has an executable recording;
  - gaps, duplicates and same-shape stream switches;
  - a policy reset versus a cut;
  - `RenderMotionScale=false`;
  - fractional motion and a subrect offset;
  - `HoldFrame`;
  - non-finite values;
  - packed counters.
- The held Cyberpunk replay with the key on must stay pixel- and decision-identical. Native Windows validation is
  required for the new pass.
- A cheaper route to a partial answer exists: the adaptive cadence probe already measures carry rejection
  through `CadenceAccept`, with its own cross-frame contract. Logging that per route measures motion against the
  model frame's proxy, not the adjacent frame's.
