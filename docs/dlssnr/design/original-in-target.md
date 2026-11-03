# Original read in place: no keep copy when the frame is edited in place

Date: 2026-11-02. Status: implemented (D3D12), host-tested, replay-identical on the GPU (2026-11-03); GPU cost not measured.
Source: the upscale-route copy audit of 2026-11-02 (candidate 2), local review notes.

## What changed

The composition's encode reads the frame and writes two textures: the proxy the model is shown (`colorCopy`) and
`hdrCopy`, the frame value for value, which the resolve composes over. When the frame is edited in place (the
after-upscale and Present routes: source and target are one resource), the target still holds that same frame when
the resolve runs: nothing writes it between the encode and the resolve, and the resolve reads each pixel of the
original at its own coordinate before writing it.

A D3D12-only permutation of `dlssnr.hlsl`, compiled with `-D NR_ORIGINAL_IN_TARGET`
(`precompile/DlssNr_OriginalInTarget_Shader.h`), uses that:

- its encode does not write `gKeep`;
- its resolve reads the original as `gTarget[id.xy]` (`NR_ORIGINAL_AT`) instead of `gOriginal.Load`.

Saved per frame: one full-frame write of the keep (3440x1440 RGBA16F: 39.6 MB). The resolve still reads the
original, now from the target. Resources, views, descriptors and barriers are unchanged; only the pipeline differs.

## When it is used

Decided once in `DlssNr_Dx12::Dispatch`, before the encode, for the encode and the resolve together. All must hold:

| Condition | Why |
|---|---|
| source is the target | otherwise the target is not the original (before-upscale) |
| `[DlssNr] Compare=0` | side by side samples the original at another pixel; a comparison switched on after the encode forces `CompareMode=0` for that one frame |
| no stabilizer, burst capture, pair capture, detail measure, pulse metric or scan meter | each reads `hdrCopy` after the resolve; each also checks the decision and skips its frame if a request arrived mid-frame |
| target format R16G16B16A16_FLOAT, R32G32B32A32_FLOAT or R11G11B10_FLOAT, with `D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD` | a float format's SRV load and typed UAV load are the same value; UNORM/sRGB views may decode differently |
| the permutation's pipeline exists | its DXIL requires *Typed UAV Load Additional Formats*, so it is built only where `D3D12_OPTIONS.TypedUAVLoadAdditionalFormats` is set |

Otherwise the default pipeline runs exactly as before. `DispatchPass` refuses a pass that asks for the permutation
without its pipeline rather than running half a frame on each. Vulkan never uses it.

## Identity of the default build

The permutation is a macro and `#ifndef`, so the default compile expands to the old source:
`DlssNr_Shader.cso` and `DlssNr_Shader_Vk.spv` rebuilt from the edited `dlssnr.hlsl` with the committed `dxc.exe`
are byte-identical to the committed ones (sha256 `3b4228d2…` and `9c5c1a29…`). The permutation's disassembly has one
`textureStore` fewer (the keep) and the *Typed UAV Load Additional Formats* flag; the default has neither change.

Rebuild (from `retoque/shaders/dlssnr/precompile`, beside the default's recipe in `retoque/dlssnr/README.md`):

```
dxc.exe -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect -D NR_ORIGINAL_IN_TARGET dlssnr.hlsl -Fo DlssNr_OriginalInTarget_Shader.cso
python create_header.py DlssNr_OriginalInTarget_Shader.cso DlssNr_OriginalInTarget_Shader.h DlssNr_OriginalInTarget_cso
```

## Tests

- `tests/nr-dispatch`: the production constructor and `DispatchPass` with fakes. The permutation is built only with
  the capability, its build failure leaves the default working, formats are allow-listed and the device's answer is
  cached, the chosen pass records the permutation's pipeline, and a pass asking for it without the pipeline
  records nothing. Source guards on the shader and on `Dispatch` (the whole gate expression, polarity and `&&`
  included, both passes, the forced `CompareMode`, every `hdrCopy` reader and their count) reject twelve mutants.
- `tests/nr-invariants`: the header is the committed bytecode.
- Replay identity on the GPU (`tests/dlssnr-loopback --replay-nr`, every variant against the same variant of the
  previous build): see the evidence section.

## Evidence

2026-11-03, RTX 4090 under Proton (vkd3d-proton), driver 615.78, model 310.9.16; DLL `8766133e` = `d2e95754` plus this
change, uncommitted at the time.

- Replay identity: `tests/dlssnr-loopback --replay-nr` on a held 93-frame Cyberpunk 2077 capture (recorded after Ray
  Reconstruction, 3440x1440 RGBA16F), variants off / cadence 1 / fixed 2 / fixed 4 / adaptive 4, each gated on the
  same variant of the 175bc188 replay of 2026-11-01 (DLL `15a8267f`; the cadence change since then replayed identical
  to it): 93/93 frames, no decision and no pixel different in any variant, `REPLAY SEAM PASS`. Each NR variant's log
  has `original read in place: available` and `original read in place, no keep copy written (3440x1440, DXGI format
  10)`. That line is logged once per process: it shows the permutation ran, not that every frame took it (no
  per-frame count was kept); the replay requested no capture, comparison or stabilizer. `off` does not run NR.
- In game: Cyberpunk 2077, after-RR route, adaptive cadence 4, about three minutes of play by the user. Same two log
  lines, NR active on the upscaler's output throughout, no NR warning or error, no Xid in the kernel log; the only
  error lines were the known Streamline `setDynamicMFGParams`/Reflex ones. Visual check by the user only.
- Not measured: GPU time before/after (the audit's estimate is 10-60 us per frame on a ~38 ms frame), native
  Windows, other games or formats (R32G32B32A32 and R11G11B10 are allowed by the gate but were not exercised).

## Not done

- Vulkan: its SPIR-V would need a storage-image read of the target's format; not attempted.
- The Record + resolve fusion (audit candidate 1) is a separate decision.
