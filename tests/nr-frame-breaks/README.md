# NR frame breaks

```bash
python3 tests/nr-frame-breaks/run.py
```

Compiles `retoque/dlssnr/DlssNr_FrameBreaks.h` under ASan/UBSan with `-Werror`, runs `cases.cpp`, then rebuilds with 14 one-line
mutants that each have to fail the cases. Design and investigation: [frame-breaks.md](../../docs/dlssnr/design/frame-breaks.md).
This checks the counting and the decision, not the picture and not a GPU.

## Cases

- what a break is: skipped frames between two frames the pass ran on; settling at the start and a stop are not breaks; a long
  gap is one; every other frame is one per gap;
- not enabled: counted, never held; enabled: three breaks start a hold of one second, it ends by time, the window starts over,
  off clears it; a healthy run with a rare skip never holds; the window forgets old breaks; totals reset;
- the reset counter: by cause in a window, the peak per second, the report cadence (nothing to say when nothing was sent), a reset
  every frame reads as about 60 a second, the ring is bounded and keeps counting.

## Source guards

`SkipStandDown` is off by default and round-trips (declaration, read, save, `OptiScaler.ini`, `Config.md`); the Vulkan route never
reads it; a skip is noted by `reportSkip`, a run by the end of `Dispatch`, a hold is neither and asks for a reset on return; the
calls of one present are one outcome; the reset is counted where it is sent to the model, once, before the evaluate; the menu
control is hidden on native Vulkan and under the proxy.

## Not covered

A game: how often admission refuses, and whether a second of plain frames is better than alternation.
