# nr-scene-cut

Host suite for scene-cut detection, phase 1 (`docs/dlssnr/design/scene-cut-detection.md`): finding a cut the game did
not flag from a 16x16 grid of its frame's light. The rule is not yet called by the renderer.

Run: `python3 tests/nr-scene-cut/run.py` (needs `g++` with C++20, ASan and UBSan).

`test.cpp` compiles the production header `retoque/dlssnr/DlssNr_SceneCut.h` and drives it with synthetic grids laid
out as the pulse pass writes them, `(luminance out, luminance in, pixels read, 0)` per tile. A world is a smooth field
of light with a sky every world shares; a frame is its tile means from a camera, with grain. It covers:

- the frame: ranks that follow the light, ties sharing their mean, levels and ranks unchanged by gain, only the game's
  frame read (channel 0, our output, scribbled over changes nothing), tiles not measured left out, too few making the
  frame unmeasured, black, flat and noise frames blank;
- the comparison: the same scene, a cut, a pan by whole tiles found as exactly that shift, by half a tile within a step,
  and nothing compared against an unmeasured or blank frame;
- sequences that must not cut: static with 1% and 8% grain, pans from 0.1 to 1.5 tiles a frame, a diagonal, a tilt,
  walking forward with and without a pan, a gain flash, an added-light flash, an explosion over a fifth of the frame
  on a shaking camera, muzzle flashes, a fade through black into another scene, a cross-dissolve, eye adaptation;
- sequences that must: a hard cut, found at its frame, and 100 world pairs cut while panning, at least 90 found and
  none found twice or elsewhere (the run prints the count and every sequence's margins);
- hysteresis: warm-up after a start and after `Reset`, cooldown after a cut and after a reset the game asked for, holes,
  pauses and late readbacks, unmeasured and blank frames;
- the request: taken once at the first frame after the cut, `Shadow` with the key off, `Late` past `kMaxLateFrames`,
  `GameAlready` when the game reset itself, and cleared;
- the log line.

`run.py` also guards the header: standard library only; `FromPulseGrid` reads channels 1 and 2 and never 0; the
header is registered in the project.

What it does not cover: real content, the GPU half, any wiring (there is none yet), or the image. Those are the A/B
items at the end of the design note.
