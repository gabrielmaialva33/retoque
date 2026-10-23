# Format swap: an RGBA8/BGRA8 swap keeps the model's history and the calibration

Status: implemented on D3D12 (`DlssNr_Dx12_Lifetime.cpp`, `ReleaseSurfacesIfFormatChanged`; the policy is
`retoque/dlssnr/DlssNr_FormatTwin.h`), tested on the host against fakes (`tests/nr-format-swap`); **not yet run in a
game**. Vulkan is unchanged.

## The problem

Some games hand DLSS their colour as `R8G8B8A8` on some frames and `B8G8R8A8` on others: Watch Dogs: Legion, reported
in janblade's fork (`758183cf`, "keep the model when the game swaps RGBA8 and BGRA8 output", 50 swaps in one tester
log). The bits are the same picture with the red and blue bytes in swapped places.

Here the model already survives that: feature 18 is never told a format, and since the FF7 Rebirth fix the
format-bound surfaces of the format being left are kept aside and swapped back (`AltSurfaces`). But every format
change, this one included, still did three things:

1. `ForgetCalibration()`: the white-point calibration starts measuring again.
2. `g_nr.reset` and every `passReset`: the model drops its temporal history, which is the visible reset.
3. The settling gates (`g_postExtent`, `g_chainExtent`) fold the format into the contract they watch, so the
   after-upscale path stood the model down for 500 ms after each swap, and a game that swaps every frame never
   settled at all. The cadence signature carries the format too, so a swap forced a model frame and, after carried
   frames, a history reset (`staleHistory`).

## Why the swap is the same picture to the pass

- **Reads.** Every view the pass builds takes the resource's own format, with typeless translated to its UNORM member
  (`kViewFormat = UNKNOWN`, `kViewTranslateTypeless`, `Shader_Dx12::TranslateTypelessFormats`). A `B8G8R8A8_UNORM` SRV
  returns red in `.r` exactly as an `R8G8B8A8_UNORM` one does; the channel order is the view's job, and no shader reads
  raw bytes. Writes through a UAV are the same in reverse.
- **History.** The model's history lives inside feature 18, in its own surfaces, filled from what the encode wrote
  through those views. Nothing of ours carries colour across frames in the game's format: `output`, `colorCopy`,
  `hdrCopy`, `colorSmall`, `outputNative` and `passPing` are rewritten every frame before they are read, and model
  cadence keeps its edit in its own surfaces (`DlssNr_Cadence_Dx12::Fits` takes no format). So the history built from
  RGBA8 frames describes the BGRA8 frame that follows, with nothing to re-type or swizzle.
- **Calibration.** The meter reads the same values through the same kind of view, so a reading taken before the swap
  describes the picture after it.

The surfaces still follow the format: `CopyResource` and `CopyTextureRegion` from the game's colour need the same
typeless family, and `R8G8B8A8` and `B8G8R8A8` are two families. The `AltSurfaces` swap stays exactly as it is; only
what it throws away changes.

## The rule

A format change keeps history and calibration only when all of these hold (`DlssNr::FormatTwin::KeepsHistory`):

- the two formats are **channel-order twins**: `R8G8B8A8_TYPELESS`/`B8G8R8A8_TYPELESS` (27/90),
  `R8G8B8A8_UNORM`/`B8G8R8A8_UNORM` (28/87), `R8G8B8A8_UNORM_SRGB`/`B8G8R8A8_UNORM_SRGB` (29/91). Each pair reads
  through the same transfer: raw UNORM for the first two, the sRGB decode for the third;
- the frame and working extents are the ones the surfaces were built for;
- the transfer the pass applies is the same: the game's linear-HDR claim (`ColourIsLinearHdr`), the present host's
  HDR white (`PresentWhite`) and its output code (`PresentQuantRel`), compared bit for bit (a NaN never matches).

Everything else resets as before:

- **UNORM against sRGB** (28/29, 87/91): the sRGB view decodes, the UNORM one does not; a different picture.
- **Typeless against typed** (27/28, 90/87): the views read alike, but no game is known to do it and the rule is kept
  to the pairs that only swap bytes. Cheap to widen with evidence.
- **`B8G8R8X8`** (88, 92, 93): alpha reads as 1, not as the game's alpha.
- **Any HDR or float format** (RGBA16F, R11G11B10, RGB10A2, ...), including the FF7 Rebirth alternation: untouched.
- **A twin swap with a size change, or with a transfer change**: resets, from this function, whatever else happens.

The game's own reset is a separate path and is not affected: `frame.Reset` (a game's cut, or the synthesized-motion
estimator's scene-cut detection, which sets the same flag) sets `g_nr.reset` before the format check runs, and the
format check only ever sets it, never clears it. A size change rebuilds the model and resets it in the structural
rebuild, as before. A placement change (before/after upscale, after ray reconstruction) resets as before.

The settling gates and the cadence signature watch the format through the same canonical form
(`ChannelOrderCanonical`: BGRA8 maps to its RGBA8 twin, everything else to itself), so a twin swap neither stands the
model down nor forces a model frame. Any other format change moves them exactly as before.

## What is still reset by a twin swap

Not part of this change, each because its own surface is copied back onto the game's texture in the game's exact
format, and each off by default:

- the output stabilizer (`DlssNr_Stabilizer_Dx12::Fits` compares the format; `StabilizerStrength` 0 by default);
- synthesized motion (`SynthMotionGuide::NeedsRebuild`; `SynthMotion` off by default);
- the frame-hold copy, the detail measurement and the pulse metric (diagnostics) restart on any format change.

A transfer change **at an unchanged format** is not a reset here, and was not before: for the 8-bit formats the
transfer cannot actually change (`FormatCanHoldLinearHdr` is false for all of them, and the HDR present host never
presents 8 bits), so no case is known where it matters.

## Acceptance

- Host (`tests/nr-format-swap`, fakes, no GPU): the production `ReleaseSurfacesIfFormatChanged` keeps `g_nr.reset`,
  every `passReset` and the calibration through 28/87, 27/90 and 29/91 swaps both ways, and resets through every other
  case above; a pending game reset survives a twin swap; the gates and the cadence signature use the canonical format;
  `frame.Reset` still sets the reset before the format check.
- Not measured: the model's output across a real swap. That needs a replay that encodes the same frame in both formats
  (the acceptance Codex asked for in the 2026-10-23 review, priority 6) or a game that swaps.
