# Format swap keeps NR history only for channel-order twins (host)

```bash
python3 tests/nr-format-swap/run.py
python3 tests/run_all.py --only nr-format-swap -v
```

Design: `docs/dlssnr/design/format-swap-history.md`.

Host only, against fakes: nothing here runs a GPU, NGX, the model or a shader, so a green run says what the lifetime
code decides, not what the model draws across a real swap.

## What runs

- **`scenarios.cpp`** compiles the production `ForgetCalibration`, `ParkAltSurfaces`, `AltSurfacesFit` and
  `ReleaseSurfacesIfFormatChanged`, cut out of `retoque/shaders/dlssnr/DlssNr_Dx12_Lifetime.cpp` by `run.py`, against
  fake resources that only answer `GetDesc` and a stand-in for the fields of `g_nr` they touch. Each scenario is a
  session settled on one format (surfaces built, no reset pending, a usable calibration) followed by frames in another,
  in the order Dispatch calls things: the format check, then the surfaces it finds missing.
- **`policy.cpp`** checks `retoque/dlssnr/DlssNr_FormatTwin.h` on its own.
- **Text guards** on Dispatch, which the scenarios cannot reach: both settling gates and the cadence signature use
  `ChannelOrderCanonical`, the format check is handed the frame's transfer built from `ColourIsLinearHdr`,
  `PresentWhite` and `PresentQuantRel`, the game's `frame.Reset` is set before the check, and the check never clears a
  reset.

## Behaviour covered

- **Kept**: 28/87, 27/90 and 29/91 swaps, both ways, then back (the set kept aside is swapped back in): no reset on the
  first pass or any other, calibration untouched. Fifty alternations of 27/90 allocate and park nothing after the
  first.
- **A game reset stands**: a reset already set on the frame of a twin swap is still set after it.
- **Reset, every pass, calibration forgotten**: a twin swap with a frame or working size change, or with a different
  HDR claim, present white or output code (a NaN on both sides counts as different); UNORM against sRGB in either
  order; typeless against typed; BGRX8; RGBA8 to RGBA16F and back; R11G11B10 to RGBA16F (FF7 Rebirth, whose kept set
  still comes back with a reset); RGB10A2 to RGBA8.
- **No change, no effect**: the same format leaves everything as it was, a pending reset included.

## Before the fix

Run on the tree before it (`294d1749`), the scenarios compile through a fallback `Transfer` and the old five-argument
call, and every "kept" case fails: 13 checks, the history reset and the forgotten calibration on each twin swap. All
the reset cases pass there too: they are the behaviour that had to stay.
