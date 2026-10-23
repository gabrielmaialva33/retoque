# Metamorphic controls of the NR input geometry (host)

```bash
python3 tests/nr-geometry-metamorphic/run.py
python3 tests/run_all.py --only nr-geometry-metamorphic -v
```

Codex's N-03 (2026-10-23): use two equivalent representations of the same scene as the oracle, so base, scale and
unit errors are found without judging the model's output. Contracts: `docs/dlssnr/design/render-geometry.md`,
`reduced-scale-guides.md`, `reduced-scale-guides-vk.md`, `format-swap-history.md`.

Host only, against fakes: no device, no NGX, no model. A green run says what the production code hands the model for
depth and motion, and what the guide resample computes, on a CPU. It says nothing about what the model draws.

## What runs

`run.py` pastes the production text into `harness.cpp.in` and compiles it with g++, C++20, ASan/UBSan, `-Werror`:

- `GatherGuideFrame` (`DlssNr_Dx12_Inputs.cpp`), against a fake NGX parameter block that also answers the MV scale's
  float keys, with the production `DlssNr_RenderSize.h` / `DlssNr_RenderGeometry.h` resolver and creation scope;
- Dispatch's geometry, reference and model-scale blocks (`DlssNr_Dx12.cpp`; the first two are the cuts
  `nr-render-geometry` makes), then Dispatch's resample-or-not branch;
- the Vulkan route's geometry and plan (`DlssNrFeature_Vk.cpp`, `EvaluateAfterUpscaleVk`, from the create flags to
  `GuideMatch::ForModel`), with `GuideMatch::Vk::MakePush` for the resample's constants;
- the guide resample itself: `PointSource` and `CSMain` from `precompile/dlssnr_guides.hlsl`, compiled as C++ on a
  small shim (`uint`, `uint2`, `Texture2D::Load`, `RWTexture2D`), every thread of every 8x8 group the dispatch
  launches. Every texel read is counted against the valid region of its texture.

The scene is analytic: an affine depth and an affine motion field (and a constant one) over normalised screen
positions. Each representation writes it into the game's guides; the harness then compares what the model is handed:
the depth image, each motion texel times the scale the model is given (pixels of the motion texture handed), and the
same in screen fractions.

## Relations

Five shapes (2x below with the resample, render under a larger model, native, 1:1, odd sizes), both routes, MVLowRes on
and off, `MatchGuides` on and off, `RenderMotionScale` on and off (legacy):

- **Exact (bit-identical model inputs)** against the canonical representation (Width/Height keys, allocations exactly
  the valid size, base zero, vectors in pixels, no MV scale key):
  - the render size through the subrect keys, or from the feature's creation through the scoped binding;
  - MV scale 1 published;
  - depth and motion at different non-zero bases inside padded allocations whose margin is a sentinel (depth 4096,
    motion -8192); padded at base zero; padded at a base with subrect keys or creation;
  - vectors x 1/8, x 4 and negated, with the MV scale compensating (power-of-two units are exact in float), and
    combined with padding;
  - the colour as BGRA8 instead of RGBA8.
- **Near (1e-5 relative)**: normalised vectors with an MV scale of the extent they are measured in, the commonest game
  convention: the reciprocal rounds.
- **Lossy (stated bound)**: MVLowRes against display-resolution vectors of the same field, display reference. Different
  sampling grids, so the screen-fraction fields are compared through the nearest coarse texel with the bound
  gradient x (half a render texel + half a display texel), plus 1e-6. Depth is the same texture both ways and must be
  identical. Under the legacy working / frame scale the relation must *fail* at the 2x-below shape: that is the fault
  fix 2 of `reduced-scale-guides.md` corrected, and the oracle has to see it.
- **Analytic**: the canonical representation against the field itself at every model texel's centre, bound half a
  texel on each grid. This catches a fault every representation shares, which no relation can.
- **D3D12 = Vulkan**: the same padded, unit-scaled representation through both routes, bit for bit.
- **A guide is bounded by its own allocation**: cutting depth's allocation short (depth then clipped) changes nothing
  about the motion the model is handed. Depth itself is not compared there: a clipped guide is not an equivalent
  representation of anything.
- **Channel order**: `FormatTwin` keeps history across an RGBA8/BGRA8 swap exactly when it would without the swap, for
  all three twin pairs and changing or unchanged sizes, and the cadence signature is the same.

Every case also requires no read outside the valid regions and no sentinel in what the model is handed.

## Mutants

Each is applied to the pasted production text (or, for the first unit mutant, to the transformation) and must make the
harness fail; one that survives, or no longer finds its text, fails the suite:

- reading the margin: the motion region as the allocation past its base (D3D12, Vulkan), the depth region ignoring
  the render size (D3D12, Vulkan), the depth base dropped, Gather ignoring the motion base, the shader ignoring the
  region base, the shader mapping over the destination size;
- a unit changed without compensating the scale: the transformation scaling vectors and not the MV scale, Gather or
  the Vulkan route dropping the game's MV scale, D3D12's scale measured against the output whatever MVLowRes says, the
  render reference taken from the allocation, the Vulkan reference ignoring MVLowRes, the Vulkan motion region taken
  from depth's clipped width.

## The defect it found (2026-10-23)

The own-allocation relation failed on the Vulkan route, 20 checks, all MVLowRes: with depth's allocation short of
base + render, `EvaluateAfterUpscaleVk` sized the low-resolution motion region and its reference from depth's *clipped*
width (`lowResolutionMotion ? guideWidth : width`), so a complete 96-wide motion texture was handed as 72 wide and
stretched over the model at 64/72 instead of 64/96. D3D12 (and so both bridges) already took the render size, as
`render-geometry.md` prescribes. The fix gives the Vulkan route the same render reference: the render size when it is
known, else what it used before. It needs inconsistent game metadata (a depth base or render size past depth's
allocation), so no shipped title is known to hit it.

## Not covered

The model, NGX, a device, the real shader compiler, D3D11 native evaluates, the pre-upscale route's scratch swap
(`nr-before-upscale`), periphery packing, Enlarge. The replay counterpart, which runs the real seam on transformed
captures, is `tools/replay/transform.py` (see `tools/replay/README.md`).
