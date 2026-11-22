# Model input format: an R32G32B32A32 scratch as an A/B (not implemented)

Status: **not implemented, on purpose.** Dated 2026-11-22. This records why a key for an R32G32B32A32 model-input scratch is
not a contained change in this renderer, what it would take, and a finding about what the model is fed today.

## The idea

NVIDIA's `vk_gltf_renderer` denoising note (nvpro-samples, Apache-2.0,
https://github.com/nvpro-samples/vk_gltf_renderer/blob/48b53acb89/docs/denoising.md) says NR expects LDR input after
tonemapping and uses an R32G32B32A32 float scratch "to avoid format conversion artefacts inside the NGX temporal buffers".
The suggestion was a cheap A/B: feed this fork's model input through an R32G32B32A32 surface instead of RGBA16F.

## What the model is fed today

It is not RGBA16F in general. Every model-facing scratch is created in **the game's own frame format** (`desc.Format`, the
target's): `g_nr.colorCopy` (the encode's proxy, the model's input when the model works at the frame's size),
`g_nr.colorSmall` (the input when it works below it), `g_nr.output` (the model's answer), `g_nr.passPing` (a chain's second
buffer), `g_nr.outputNative` (the supersampling down-leg) and `g_nr.hdrCopy` (the untouched copy the resolve composes against)
(`DlssNr_Dx12.cpp`, "mandatory NR scratch initialization", `CreateScratch(device, desc.Format, ...)`). So an HDR game that
hands the pass RGBA16F feeds the model RGBA16F, and an SDR game whose output is R8G8B8A8_UNORM feeds the model, and takes its
answer back through, **8-bit** surfaces; R10G10B10A2 and R11G11B10 are the same story at their own precision. That is a
plausible contributor to low-amplitude flicker and banding on SDR titles, and it is worth measuring before anything else in
this note: the A/B that matters is "the game's 8-bit format against a float surface", not "RGBA16F against RGBA32F".
Not measured here.

## Why a key is not contained

The format is not a property of one surface. It is the contract of a bundle, and the bundle is keyed to `desc.Format` in at
least these places:

- `scratchContract` (width, height, working size and `desc.Format`) and its retry and retirement
  (`retireMismatch` compares every member against `desc.Format`);
- `ReleaseSurfacesIfFormatChanged`, `AltSurfacesFit` and the alt set kept for the other format: the 2026-09-29 swap of
  RGBA8/BGRA8 and R11G11B10/RGBA16F on cutscene cuts (`format-swap-history.md`, `DlssNr_FormatTwin.h`) is decided on
  `desc.Format` and on the twin of it, and keeps the model's history through the swap;
- `OriginalInTargetUsable(desc.Format)`: the keepless permutation reads the frame from the target itself, which needs the
  scratch and the target to agree;
- the chain's admission estimate (`surfaceBytes`, a fixed 48 bytes a pixel), the cadence and stabilizer and detail stats
  textures that are sized and typed against the output, the pair capture's row conversions;
- the model's own surfaces: the feature is never told a format, so an answer surface of another format changes what the
  model writes into, which nothing here has validated for R32G32B32A32 on this model.

A key that changed only `colorCopy` would put an RGBA32F proxy next to an 8-bit answer and an 8-bit original: the resolve's
`gOriginal`/`gTarget` reads and every `desc.Format` comparison would disagree with it. A key that changed the whole bundle is
the format contract, the retirement path, the alt-surface machinery and the admission estimate at once, on a path that must not
change when the key is off. Memory also changes by size, not by a constant: an RGBA32F working surface is 16 bytes a pixel
(128 MB at 4K for each of the proxy, the answer and the copy). And the result could not be judged without a GPU and a game.

## What it would take

1. Decide the format once, in one place, before `scratchContract`: `modelFormat = key ? R32G32B32A32_FLOAT : desc.Format`,
   and use it for the model-facing members only (`colorCopy`, `colorSmall`, `output`, `passPing`, `outputNative`), leaving
   `hdrCopy` and the target as the game's.
2. Add `modelFormat` to the contract, the retirement comparisons and `AltSurfacesFit`, and decide what the twin machinery does
   when the model format is not the frame's (nothing to swap: the model format would not move).
3. Make `OriginalInTargetUsable` and the chain admission aware of it.
4. Validate with the loopback fixture on native Windows that the model accepts the surface in and out at all.

None of that is a key; it is a change to the renderer's format model. The cheaper first step is the measurement the finding
above suggests: log, once, the format of the model-facing surfaces per session (it is already `desc.Format`) and compare the
flicker of one SDR title against an HDR one with "Measure detail". If that shows 8-bit input to be the problem, this note is the
plan.
