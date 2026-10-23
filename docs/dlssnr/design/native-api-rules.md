# DLSS-NR: the fork's D3D12 and Vulkan code against the API rules (audit, 2026-10-23)

## Why

vkd3d-proton accepts D3D12 calls that native Windows answers by removing the device. The guide resample's
48-byte constant-buffer view ran on Linux and removed a native RTX 3060's device at the first frame
(`13f10d60`, [reduced-scale-guides.md](reduced-scale-guides.md) "Native Windows"); an sRGB unordered-access
view did the same in Metal: Hellsinger (`DlssNr_PresentHost.cpp`, 2026-10-14). Users of this fork run native
Windows too, so everything the fork owns was read against the D3D12 and Vulkan specifications, not against what
ran here.

Scope: `retoque/shaders/dlssnr/` (every pass and `DlssNr_Dx12*.cpp`), `retoque/dlssnr/` (the present hosts, the HDR
present, the exposure scan, the direct loader, DRED, timing, capture and replay), `retoque/shaders/synth_motion/`,
and the D3D11 bridge where retoque code hands it resources. `retoque/lsfg/` was not in scope.

Method: four read-only passes over those files, with each finding then checked against the code by hand. The
committed bytecode was read directly. Native `dxc -dumpbin` crashes on these containers, so the DXIL PSV0
(bindings) and SFI0 (feature flags) parts were decoded in Python. The SPIR-V went through `spirv-dis`,
`spirv-val` and `spirv-cross --reflect`. Nothing here ran on native Windows or on any GPU. A finding is a reading
of code against a rule.

Severity:
- **P1:** device removal, a crash or undefined behaviour on native.
- **P2:** a spec violation every current driver is known or expected to tolerate.
- **P3:** hygiene.

## Fixed on `claude/native-rules-audit-20261023`

| Sev | Where | Rule | What was wrong | Fix | Test |
|---|---|---|---|---|---|
| P1 | `DlssNr_PresentHost.cpp` `AbandonRecording` → `_ReleaseBuilt` | An object a submitted command list references must outlive its execution (debug layer: OBJECT_DELETED_WHILE_STILL_IN_USE) | The bridge abandons a frame whose list would not close. The host then freed the sRGB stage, the zero guides and their heaps, both format transfers and the setup lists at once. The frames before it, on the same queue, may still have been reading them. | A fence signalled on the last Record's queue is waited on for up to 5 s before the rebuild. If the queue cannot be shown idle, the host keeps everything and declines for the session; Release frees it after the bridge's teardown drain. | `nr-present-host`: a busy fake GPU asserts on any release, and a hung one must free nothing (red before). |
| P1 | `DlssNr_Dx12.cpp` `DispatchPass` and `BindKey` | A descriptor must name a live resource whenever the GPU reads it | Descriptor reuse keyed a game's resource on pointer and shape. If that resource was released and re-created at the same address with the same description, every ring slot matched and kept a descriptor naming freed memory. [descriptor-reuse.md](descriptor-reuse.md) recorded this as the residual risk. | `BindToken`: a 64-bit token of ours, kept on each bound resource as private data (`SetPrivateData`), is part of the key. A new object at an old address carries none and gets a new one. A resource that refuses the token is never reused. | `nr-dispatch`: a replaced game resource must have its views written again (red before). |
| P1 by rule, P2 in practice | `SynthMotion_Dx12.cpp` `_EnsureDevice` | "Typed unordered access view loads": past R32_FLOAT/UINT/SINT and the guaranteed set, each format is optional, so ask `D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD` | The search, filter, scale and expand passes read R16G16_SINT flow and R8_UINT luma through typed UAVs (their SFI0 carries TypedUAVLoadAdditionalFormats). Nothing asked whether the device can. | `CheckFeatureSupport` for typed UAV load and store on both formats before the pipelines are built, otherwise no field for the session. No current desktop GPU is known to lack them. | `nr-native-rules` check 7 (red before). |
| P2 | `DlssNr_Dx12.cpp` `RunPresentPass`, the dummy guides | `StateBefore` and the state at use must be the resource's real state | `ZeroGuides::RecordClear` records a one-way UNORDERED_ACCESS → NON_PIXEL_SHADER_RESOURCE transition once. The present route never said whether that list ran. A dropped list left the guides believing the clear was recorded, and every later frame handed NGX "NON_PIXEL_SHADER_RESOURCE" guides that were still in UNORDERED_ACCESS. | Confirmed when the list executed and abandoned when it was dropped, as the synthesized motion already is. | `nr-native-rules` check 8 (red before). |
| P3 | `SynthMotion_Vk.cpp` module creation | `VUID-VkShaderModuleCreateInfo-pCode-parameter`: `pCode` points to `uint32_t` | A byte-aligned `unsigned char[]` was cast to `const uint32_t*`. | The bytes are copied into a `std::vector<uint32_t>` first. | `nr-native-rules` check 9. |

None of these changes rendered output on a frame that ran correctly before. All five are covered only by host
fakes and static checks.

## Open, for review

Not fixed here: each one changes pixels, needs a shader rebuild, or touches a file another worker owns.

| Sev | Where | Rule | Finding | Proposed fix | Pixels |
|---|---|---|---|---|---|
| **P1** (conf. medium: reachable on Unreal-style depth) | `DlssNr_Dx12_Resources.cpp` `TypedGuideFormat` / `CreateGuideClone` | `CopyResource` needs one format family, size and plane count. A fully typed resource is viewed only in its own format, and a `D*` format is never an SRV format. | Planar typeless depth (`R24G8_TYPELESS`, `R32G8X24_TYPELESS`) has no valid clone among the candidates. `R24_UNORM_X8_TYPELESS` / `R32_FLOAT_X8X24_TYPELESS` are view formats that creation refuses. `D24_UNORM_S8_UINT` / `D32_FLOAT_S8X24_UINT` with no flags leaves a resource nothing can make an SRV of (GuideMatch's view and NGX's fail on native). `R32_FLOAT` crosses families, texel size and plane count in `CopyResource`. vkd3d-proton accepts the second. `nr-present-hook.md` lists D24S8/D32S8 as untested on native. | Don't copy planar depth. Read plane 0 through `R32_FLOAT_X8X24_TYPELESS` / `R24_UNORM_X8_TYPELESS` (PlaneSlice 0) on the typeless source, in a small compute pass writing R32_FLOAT. The guide resample already reads depth that way and could run at 1:1. | Should be none where Proton is right today; must be measured |
| P1 if hit (conf. medium) | `DlssNr_Dx12.cpp` `EnsureCompose` and the `g_nr` scratch | Every object on a command list belongs to that list's device | The composition's root signature, PSO, heaps, constants and scratch are built on the first device and never rebuilt for another. Only the pre-upscale path compares devices. A title that recreates its D3D12 device (TDR recovery, adapter change) records device-A objects on device-B lists. | Decline or rebuild when `target->GetDevice()` differs, as `PresentHdr`'s `BuiltFor` does. | None |
| P2 (reachability low; UB where hit) | `OptiScaler/shaders/Shader_Dx12.cpp:37-38` (upstream), used with `kViewTranslateTypeless = true` by every NR pass | The view's component type must match the HLSL declaration (`float4`) | `R10G10B10A2_TYPELESS` maps to `R10G10B10A2_UINT`. A target or scratch in that format is read and written through UINT views by float shaders. No title is known to hand NR such an output. | An NR-owned mapping to `R10G10B10A2_UNORM`, leaving upstream's table alone, or decline typeless outputs outside a known list. | Yes, from garbage to correct, only where hit |
| P2 (NVIDIA tolerates) | `dlssnr.hlsl:239,243` (`gTarget`, `gKeep`), `dlssnr_present_hdr.hlsl:47` (`gOut`): SPIR-V `OpTypeImage ... 2 Rgba32f` | Vulkan "Compatibility between SPIR-V image formats and Vulkan formats": a storage image declared with a format is accessed through a view of that format | Every view bound there is RGBA16F, R32_SFLOAT, A2B10G10R10 or the game's own output format. The code believes the opposite: `DlssNr_VkPresentNr.h` says the SPIR-V "writes storage images without a format", and `DlssNr_PresentHdr_Vk.h` checks `STORAGE_WRITE_WITHOUT_FORMAT_BIT`. No validation-layer VUID catches it at dispatch. | Rebuild both Vulkan targets with `-fspv-use-unknown-image-format`. Then make the after-upscale device satisfy `StorageImageWriteWithoutFormat` (feature, Vulkan 1.3 or `VK_KHR_format_feature_flags2`, VUID-VkShaderModuleCreateInfo-pCode-08740), and update the nr-invariants hashes. | None expected on NVIDIA; must be measured |
| P2 (opt-in diagnostic, off by default) | `DlssNr_ExposureScan.cpp:628,655` (report only, another worker's file) | `StateBefore` must be the real state; non-simultaneous textures do not decay to COMMON | `Tick` hard-codes UNORDERED_ACCESS → COPY_SOURCE → UNORDERED_ACCESS on the game's candidates. An exposure texture DLSS reads is usually in NON_PIXEL_SHADER_RESOURCE, and a buffer written in an earlier list has decayed to COMMON. The comment there calls it an assumption. Also `:316`: buffers of 1-3 bytes pass the filter and are copied 4 bytes (`CopyBufferRegion` past `Width`). `:216` uses ALL_SUBRESOURCES on candidates that may have mips. | Copy only resources with a declared state, rely on COMMON promotion for buffers, reject `Width < 4`, and use subresource 0. | Only the white point, when the scan is its source |
| P2 (to verify) | `DlssNrFeature_Vk.cpp:507` `Transition` on the after-upscale route (report only) | VUID-VkImageMemoryBarrier-oldLayout-01197 | Tracked layouts of `proxy`, `keep`, `output`, `proxySmall` and `meter` carry across the game's command buffers. A buffer the game drops or re-records leaves the next `oldLayout` wrong. The present route resets them every frame. | Start each evaluate from UNDEFINED; all are fully rewritten each frame. | None |
| P2 (to verify) | `DlssNr_Vk.cpp`, `DlssNr_Vk.h` sets ring (18) on the after-upscale route | VUID-vkUpdateDescriptorSets-None-03047 | Up to four dispatches a frame reuse 18 sets and constant slots with no completion gate, so a slot wraps after about 4.5 frames of pending work. The present route gates in `Admit`. | Gate on completion. | Only when it fires |
| P2 (to verify) | `DlssNr_Readback.h:26-31`, `DlssNr_ReplayRecorder.cpp:316` | Placed footprints of typeless formats | The header says a typeless placed footprint "removes the device". `TypedForCopy` maps only five formats, so R32, R16G16, R32G32 and R24G8 typeless still get typeless footprints. Either that explanation is wrong, or replaying such a depth or motion hits the same removal. | Settle which, then map every family or refuse. | None |
| P3 | Descriptor and constant rings with no fence | Root signature 1.1 defaults (`DESCRIPTORS_STATIC`, `DATA_STATIC_WHILE_SET_AT_EXECUTE`) | Rings of descriptor heaps and constant slots are rewritten with no completion check. They wrap only if the GPU falls a whole ring behind: the main pass 64 slots at ≤ 6 dispatches/evaluate, Cadence 112 at 7, Periphery 32 at 3, GuideMatch/Stabilizer/UiMask 16; synthesized motion's 8/16-slot volatile rings. | Gate slot reuse on `DlssNr::Submission`, as Pulse and DetailStats do. | None |
| P3 | `DlssNr_Pulse_Dx12.cpp` `Light()`, `DlssNr_DetailStats_Dx12.cpp` `Readback()`; `SynthMotion_Dx12.cpp` and the exposure scan's readback rings | `Map` with a read range invalidates the CPU cache; reads of GPU writes wait on a fence | Mapped for life and read with no Map, or read three submitted (not completed) frames later. This relies on coherent readback memory, as on x86 discrete GPUs. | Map/Unmap with the read range, and a fence per slot. | Rarely, a one-frame-late value |
| P3 | `DlssNr_Dx12.cpp` `EnsureBbCopy` | Typed UAV store support | No `UAV_TYPED_STORE` check on the SDR backbuffer format. An unsupported format fails creation and the pass latches off. | Ask, and decline with a reason. | None |
| P3 | `DlssNr_Dx12_Resources.cpp` `ReadableGuide` | `CopyResource` within one family | A clone is kept when only the source's format changes at the same shape. | Put the family in the rebuild test. | None |
| P3 | `DlssNr_PresentHost.cpp:345` | `StateBefore` is the real state | `_featureAttempted` is set before the setup lists exist. An allocation or `Close` failure there leaves the guides in UNORDERED_ACCESS while the pass is told they rest in `GuideRestState()`. | Set it once list 0 executed. | None |
| P3 | `SynthMotionNvofa_Dx12.cpp:654-664` | A TEXTURE2D SRV on an MSAA or non-2D resource is invalid | No dimension/sample check, unlike the FFX and overlay paths. The present colour cannot be MSAA today. | The same check. | None |
| P3 | `dlssnr_present_hdr.hlsl:142` `[numthreads(16,16,1)]`, `DlssNr_PresentHdr_Vk.h` `Supports()` | VUID-RuntimeSpirv-x-06432: invocations ≤ `maxComputeWorkGroupInvocations` (128 guaranteed) | 256 invocations, not checked. NVIDIA allows 1024. | Check the limit. | None |

Speculative leads that could not be settled by reading:
- `ExposureScan::ReleaseTrackedResources` is called from `NVSDK_NGX_D3D12_ReleaseFeature` with no GPU wait.
- `NoteResource` does not compare devices.
- The bridge's shared copy can lack `D3D11_BIND_SHADER_RESOURCE`, so whether the D3D12 SRV made of it is legal is open.
- NvOFA's output is created with no flags, in COMMON.

## Reviewed clean

- **CBVs.** Every view is 256-byte sized (`alignas(256)` structs) and sits at its own buffer's base address. GuideMatch's `13f10d60` fix is complete.
- **Root signatures.**
  - Every NR pass is one table costing 1 DWORD, with SRV/UAV/CBV ranges and static samplers only. No SAMPLER range is mixed into a table.
  - Synthesized motion costs 18, 14 and 18 DWORDs, and its root-constant writes stay inside their parameters.
  - All 145 DXIL bindings of the 12 passes lie in their signatures.
- **Descriptors.**
  - One shader-visible CBV_SRV_UAV heap is set per dispatch, and every table handle comes from it.
  - `ClearUnorderedAccessViewFloat` takes a GPU handle from the set heap and a CPU handle from a non-visible one.
  - `CopyDescriptorsSimple` copies from non-visible heaps only.
  - Every slot is written, with stand-ins where a mode has nothing of its own.
- **Formats.** No UAV is created on an sRGB or typeless format, and no UAV or SRV on a swapchain buffer (D3D12 copies in and out; Vulkan only copies). The present host asks `SHADER_LOAD` + `TYPED_UNORDERED_ACCESS_VIEW` + `UAV_TYPED_STORE`. No NR DXIL carries TypedUAVLoadAdditionalFormats; the only RW read is Cadence's R32_FLOAT state.
- **States.**
  - UPLOAD stays GENERIC_READ and READBACK stays COPY_DEST.
  - No transition goes from a state to itself.
  - Every write→read crosses a transition or a UAV barrier.
  - Compute lists use only compute-legal states, and game resources leave in the state they came in.
  - The D3D11 bridge's shared textures cross in COMMON behind a D3D11 Signal → D3D12 Wait.
- **Copies and footprints.**
  - Footprints come from `GetCopyableFootprints`, or are hand-built 512/256-aligned.
  - `CopyResource` shapes match.
  - Timestamp queries run on DIRECT lists only, and resolve into READBACK in COPY_DEST.
- **Vulkan.**
  - Push-constant blocks fit their 64-byte range and 128.
  - Set layouts and pools match the reflection.
  - Synthesized motion's declared formats match their views, with format features checked.
  - Swapchain images are copied, not viewed, with TRANSFER usage added only after `supportedUsageFlags` and image-format checks.
  - Queue-family transfers (DOOM Eternal's compute present) are released and acquired pairwise.
  - Memory is dedicated, at offset 0, with the type taken from `memoryTypeBits`.
  - All 12 modules pass `spirv-val`.

## Held mechanically

[`tests/nr-native-rules`](../../../tests/nr-native-rules/README.md) now holds the rules a script can read off the
source and the committed bytecode:
- DXIL bindings inside the root signature;
- root cost, table and constant writes;
- upload/readback states;
- no transition from a state to itself;
- CBV placement, including the aggregate form `nr-invariants` never saw;
- SPIR-V push constants and format-less storage;
- typed UAV loads beyond R32 only behind a format check;
- every zero-guide clear settled both ways;
- no byte array cast to `pCode`.

Each check fails on a violation seeded into a copy of a real pass first.

Two open findings are also mechanical and are held as known exceptions with their reasons (checks 10 and 11): the
`R10G10B10A2_TYPELESS` integer view, and the two Vulkan modules' `Rgba32f` storage declarations. A new instance fails,
and so does a fixed one until it is taken off the list.

The open findings above are not mechanical, since they depend on runtime values, and stay with review
([DEVELOPMENT.md](DEVELOPMENT.md) §3). A native-Windows run is still the only evidence for any of them, the fixed
ones included.
