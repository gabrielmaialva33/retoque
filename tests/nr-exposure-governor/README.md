# NR exposure governor

```bash
python3 tests/nr-exposure-governor/run.py
```

Compiles `retoque/dlssnr/DlssNr_ExposureGovernor.h` with g++ C++20 under ASan/UBSan and `-Werror`, runs
`cases.cpp`, then rebuilds it with ten one-line mutants of the rule, each of which has to fail the cases. Design:
[exposure-governor.md](../../docs/dlssnr/design/exposure-governor.md). This checks the rule and where it is called,
not the picture and not a GPU.

## What the cases cover

- the first reading is taken as it is; a target inside the 0.15 stop band moves nothing;
- the rate is per second at 30, 60 and 144 fps (4 stops/s up, 0.5 down), reaches the target without passing it, and
  two evaluates of one frame (no time between them) do not step twice;
- hysteresis: engage at 0.15 stop, keep tracking until within 0.02;
- an unreadable frame holds and keeps the hysteresis state, and the scene after it is followed at the limit;
- a start on an unreadable frame passes the target and takes the first readable one; the game's reset re-primes, and
  on an unreadable frame waits for the first readable one; a pause over 2 s re-primes; a long frame inside it is
  bounded to 0.25 s of step;
- NaN, infinite, zero and negative targets pass untouched; negative or NaN time is no time;
- the hold's range equals `PlausibilityLatch`'s, at compile time;
- a 16 stop fade up and down never overshoots and falls no faster than the release rate.

## What the source guards pin

The key is off by default and round-trips (declaration, read, save, shipped `OptiScaler.ini` and `Config.md`); the
governor is called once, after `ResolveWhitePoint` and before the shader's live read is decided, which is off while the
key is on; the game's reset re-primes and the zero-guide policy reset does not; the only write to `whitePoint` between
the two is inside the key's branch (off is byte for byte the old white point); the menu control is under the game's
exposure source and hidden on Vulkan; the Vulkan route never reads the key.

## Not covered

The picture, a GPU, a game, or the readback lag. Whether `[0.02, 512]` catches a real game's black frame.
