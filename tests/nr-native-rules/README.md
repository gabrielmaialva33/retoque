# NR native API rules

Run `python3 tests/nr-native-rules/run.py`. Pure Python: no compiler, no Wine prefix, no GPU, so it is
safe beside a running build.

vkd3d-proton accepts D3D12 calls that native Windows answers by removing the device. The guide
resample's 48-byte constant-buffer view ran on Linux and removed a native RTX 3060's device at the first
frame (`13f10d60`, `docs/dlssnr/design/reduced-scale-guides.md` "Native Windows"). This suite holds the
rules that can be read off the source and the committed bytecode, so finding them does not wait for a run
on someone's Windows machine. The audit it came from, with the findings that are not mechanical, is
[`docs/dlssnr/design/native-api-rules.md`](../../docs/dlssnr/design/native-api-rules.md).

| # | Rule | Read from |
|---|---|---|
| 1 | Every resource a pass's DXIL declares lies in a range of the root signature that pass creates (type, register, space); a sampler in a static sampler or a sampler range. A shader binding outside its root signature makes `CreateComputePipelineState` fail on native. | PSV0 part of the DXIL container, decoded from the `*_Shader.h` byte array the build embeds; `SetupRootSignature(...)` (upstream's one-table layout) or a `CD3DX12_ROOT_PARAMETER1` array |
| 2 | A root signature costs at most 64 DWORDs; `SetComputeRoot32BitConstants` writes inside its parameter's `Num32BitValues` and only into a constants parameter; `SetComputeRootDescriptorTable` only into a table; no table mixes SAMPLER with CBV/SRV/UAV ranges. | the same |
| 3 | A committed resource on an UPLOAD heap is created in `GENERIC_READ`, on a READBACK heap in `COPY_DEST` (`D3D12_HEAP_TYPE`). | `CreateCommittedResource` calls and the heap properties declared nearest before them |
| 4 | A transition names two different states, where both are literals (`D3D12_RESOURCE_TRANSITION_BARRIER`). | `CD3DX12_RESOURCE_BARRIER::Transition` and every local transition/barrier helper |
| 5 | A CBV's `BufferLocation` is a resource's own GPU address; an offset has to show its 256-byte placement (`D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT`). `nr-invariants` check 5 holds the size of `SizeInBytes = sizeof(T)`; the aggregate form `D3D12_CONSTANT_BUFFER_VIEW_DESC v { address, sizeof(T) }` (the cadence probe), which that regex never saw, is held here to the same rule: `T` declared `alignas(256)`, or a literal multiple of 256. | `BufferLocation =` assignments and aggregate CBV descriptors |
| 7 | A pass whose DXIL carries SFI0's `TypedUAVLoadAdditionalFormats` (a typed UAV load of more than one 32-bit component) asks `CheckFeatureSupport` for `D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD`: past R32_FLOAT/UINT/SINT and the guaranteed set, a format such as `R16G16_SINT` is supported per format ("Typed unordered access view loads"). Found the synthesized-motion estimator loading its R16G16_SINT flow without asking. | SFI0 part of the DXIL container, and the pass's source |
| 8 | Every object a file calls `ZeroGuides::RecordClear` on (a one-way UNORDERED_ACCESS → rest-state transition recorded once) is answered in the same file with `ConfirmExecuted` and `AbandonRecording`. Otherwise a dropped list leaves the guides believed to be in a state they never reached, and every later `StateBefore` (and NGX's input state) is wrong. Found the present route never settling its dummy guides. | `RecordClear` call sites under `retoque/` |
| 9 | `VkShaderModuleCreateInfo::pCode` is never a `create_header.py` byte array cast to `const uint32_t*` (it must point to `uint32_t`s, `VUID-VkShaderModuleCreateInfo-pCode-parameter`; the arrays are byte-aligned). Found the synthesized-motion estimator's modules created that way. | `pCode =` assignments |
| 6 | Each committed `.spv`'s push-constant block fits 128 bytes (the guaranteed `maxPushConstantsSize`) at 4-byte offsets; a storage image of format `Unknown` that is read or written carries the capability and its loader checks `shaderStorageImageRead/WriteWithoutFormat`; with `spirv-val` installed, every module validates for Vulkan 1.1. | the SPIR-V words |

Scope: `retoque/shaders/dlssnr/` (and `motion/`), `retoque/shaders/synth_motion/` for checks 1-2, 6 and 7;
those plus `retoque/dlssnr/` for 3-5.

Each check is run first against copies of real passes with one violation planted
(`DlssNr_Cadence_Dx12.cpp`, `SynthMotion_Dx12.cpp`, `SynthMotion_Expand_Shader_Vk.spv`,
`DlssNr_Shader_Vk.spv`), and the unchanged copies must pass: a pattern that stopped matching the code would
otherwise read as a pass. The live run also fails when it reads fewer passes, bindings, heaps, transitions,
CBVs or modules than the tree has, for the same reason.

## What this does not cover

Everything that depends on runtime values: whether a `StateBefore` is the state the resource is really in,
whether a UAV barrier separates a write from the read that follows, which DXGI/Vulkan format a view gets and
whether that format supports typed UAV loads or storage, queue-family ownership, layout history. Those are in
the audit note as findings or as reviewed-clean, not here. Nothing here runs a shader or a GPU.
