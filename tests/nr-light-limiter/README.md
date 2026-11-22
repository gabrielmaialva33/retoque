# NR light limiter at any cadence

```bash
python3 tests/nr-light-limiter/run.py
```

Compiles `retoque/dlssnr/DlssNr_LightLimit.h` and the shader's own rule header
(`dlssnr_cadence_rule.h`: `CadenceLimitLight`, `CadenceLimitRegion`, `CadenceApplyGain`, `CadenceLight`) with g++ C++20
under ASan/UBSan and `-Werror`, runs `cases.cpp`, then rebuilds with sixteen one-line mutants, each of which has to fail
the cases. Design: [light-limiter.md](../../docs/dlssnr/design/light-limiter.md). This checks decisions and where they
are called; it does not run the shader or a GPU.

## What the cases cover

- `Requested`: only 1 and 2 are modes; a typo is off. The regional step is half the global rate, clamped as the cadence's.
- The gate: first frame, hold to the frame before, a second call in one present, a reset of the model's history, a
  changed signature, a changed mode, a gap over 250 ms, the cadence's own limiter holding a frame, a frame that could
  not be limited, off in between, and a route without a present count.
- `CadenceLimitRegion`: nothing before it or not finite passes; held to one step a frame either way; untouched inside the
  step; let go at 0.15 stop of the tile's own light; let go at the motion threshold, held just under, a non-finite motion
  reads as still; the answer's drift released between 0.30 and 0.40 stop, monotonically; the gain clamp.
- A CPU model of the three regional dispatches in the host's order (tile sums, one step a tile, bilinear gain): lamp
  breathing in one tile of a still picture is cut to a quarter of the swing plus three quarters of the step (a tent, not
  a box) and the other tiles are untouched; the contrast of a texture inside the tile is the answer's own to 0.01 stop; a
  real change of the picture's own light goes through at once; a reset shows the answer as it is; a black picture is
  neither held nor poisoned.
- The cadence's global rule is unchanged.

## What the source guards pin

The key is off by default and round-trips (declaration, read, save, `OptiScaler.ini`, `Config.md`); the Vulkan route never
reads it; modes 10 to 12 exist in the shader and the host; the cbuffer grew at its end only, in order; the state is R32F;
the light-only object builds none of the cadence's surfaces and every cadence call refuses it; the renderer builds the
object once, only after the gate says run, parks it when the key is off, runs it after the cadence's record and fade and
before the down-leg, the unpack and the resolve, which read `shownAnswer`; the model's own surface is not written; the object
is parked with the generation and deleted at shutdown; the menu control is hidden on native Vulkan, under the proxy and
without the shader.

## Not covered

The GPU: typed UAV loads from R32F, the group-shared reductions, the dispatch sizes. The picture. A game.
