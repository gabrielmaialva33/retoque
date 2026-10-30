# LSFG on D3D12 (study, branch study/lsfg-d3d12)

Running Lossless Scaling's frame generation (LSFG 3.1) on D3D12 inside OptiScaler, from the user's own Lossless Scaling
install (`lsfg-vk` beta branch, `lsfg-vk.dll`). Generated files go to `Tmp/lsfg-study/` (outside git).

1. `extract.py <lsfg-vk.dll> Tmp/lsfg-study/spv`: the 99 SPIR-V modules, by resource id (0 mipmaps, 1/2 generate
   with 8-/16-bit output, then 24 core shaders at 3..26 with performance +24 and FP16 +48).
2. `convert.py Tmp/lsfg-study/spv Tmp/lsfg-study`: spirv-cross to HLSL SM 6.0, each Vulkan binding moved to its own
   register space (binding N -> `register(x0, spaceN)`, so arrays don't overlap), dxc to DXIL. `layout.json` holds the
   per-shader binding map for the root signatures. Status 2026-10-13: 99/99 compile.
3. Next: a D3D12 harness that runs the LSFG 3.1 pass graph on two frames and writes the generated one, checked
   against `lsfg-vk-cli debug` on the same frames.

Needs: `spirv-cross` (Arch extra), `uvx --with pefile`, dxc from `OptiScaler/shaders/shader_tools/`.

`convert.py --fp16-native` also writes SM 6.2 native 16-bit conversions of the FP16 modules (ids 51..98) to
`<target>/fp16-native/`, for study. Nothing reads them: `FP16_NOTES.md` section 7 says why.

`derive_ids.py [lsfg-vk.dll]` re-derives the id -> role table of `retoque/lsfg/LsfgShaderLibrary.cpp` without any id
table and exits 1 if they differ. It reads the DLL in memory and prints only interface facts. The roles are lsfg-vk's
GPL-3.0-or-later revision `8b0da266` and MIT `v1.0.0` (`Licenses/lsfg-vk_ATTRIBUTION.txt`), which number shaders by
Lossless.dll's ids; the ids come from the DLL's own bindings, formats, Float16 capability and image-access counts.
Retoque's `epsilon0..4` is lsfg-vk's `delta[5..9]`. The quality/performance split of `beta1..4` has no fingerprint in
either source and follows the block layout the other 20 core roles establish. `tests/lsfg-shader-library` runs it
when the DLL is installed.

Provenance: this study was first written against lsfg-vk 2.0.0 (CC BY-NC-ND 4.0). The id table, `LsfgGraph.h` and
the samplers now cite the revisions above and the user's DLL; `PIPELINE_V3_1.md` still uses 2.0.0's vocabulary and
its banner lists what remains (`OPTI_INTEGRATION.md`, "Read this first").
