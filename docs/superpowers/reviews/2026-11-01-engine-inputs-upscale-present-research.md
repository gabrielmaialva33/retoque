# Engine inputs and route-specific optimization for RTX 4090

Research note — 2026-11-01. Code baseline: `73fad086eef72bb2e07845d751c9d6d2d514425c` on `dlss-neural-rendering`.

Status: source audit and primary-source research. This note proposes work; it does not implement new modes or report new performance/quality measurements. The existing cadence candidate and its deployment are outside this investigation. No build, game run, INI change, deployment, commit or merge was performed for this study.

## Findings that determine the direction

Retoque already receives useful engine inputs through upscaler APIs. D3D12 Present already has owned depth/motion captures, a four-slot ring, submission tracking and enforced identity rules. Improving information flow means preserving the meaning, frame association and lifetime of those inputs, then consuming only the fields for which the NR path has an established use.

The largest architectural opportunity is a small, explicit input contract shared by the route adapters. It should extend the existing `DlssNrFrameInfo`, render-geometry and present-identity machinery. It should not replace them with a second generic hook framework.

Three different facts must remain distinguishable: a game supplied a field; Retoque actually forwarded or used it; changing that field affected the model or final output. A recognized tag or a successful parameter readback proves neither visual effect nor performance improvement.

Some performance candidates are specific to pre-upscale, SDR passthrough or guide resampling. They do not establish a large saving for the current HDR post-RR path at working scale 1.0. The model itself remains behind the NGX runtime; this repository controls preparation, scheduling, resources, composition and invocation.

## 1. Current routes

| Route | What the current code does | Important boundary |
|---|---|---|
| D3D12 after SR | `EvaluateAfterUpscale` obtains output/depth/motion, gathers metadata, then dispatches NR | A point inside upscaler evaluation; generally earlier than final game post-processing and HUD. The precise game pipeline still matters |
| D3D12 after RR | Uses the denoised output when `ApplyAfterRR` permits it; separate RR scale/pass policy | Pre-RR noisy input is not interchangeable with the denoised result |
| D3D12 before SR | `ScopedPreUpscale` lends an edited private render-size color to one upscaler evaluation, then restores the original binding | Experimental; raw jittered input, supported formats/rects, failure restoration and history require their own validation |
| D3D12 Present | Captures depth and motion when this route is the consumer, chooses an eligible capture, copies the backbuffer to work on it, then copies the result back | Source is the final presented image; game post-processing can change its spatial relationship to the guides |
| D3D11 bridge Present | Converts through its work texture; native D3D11 DLSS guides can be transported to the D3D12 ring | Transport/conversion and synchronization add work beyond model evaluation |
| Native Vulkan after upscale | Has its own preparation in `EvaluateAfterUpscaleVk` | It does not use `DlssNrFrameInfo`; D3D12 stage/RR policy must not be assumed to apply unchanged |
| Native Vulkan Present | Work-image round trip, zero guides or synthesized motion; current FG policy can stand down | No real-guide ring in this route today; adding one requires Vulkan-specific lifetime and synchronization |

These placements are alternatives, not instructions to run NR at every stage. Existing arbitration also stands down when the game is already running its own Streamline NR.

Code anchors:

- `OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp`: evaluate entry and pre/post scopes.
- `retoque/shaders/dlssnr/DlssNr_Dx12.cpp`: `EvaluateAfterUpscale`, `ScopedPreUpscale`, `RunPresentPass`, `PresentFrameDefaults`.
- `retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp`: `CaptureTemporal`, `CaptureGuides`, `ReadCapturedGuides`.
- `retoque/dlssnr/DlssNr_PresentGuideRing.h`, `DlssNr_PresentGuideIdentity.h`, `DlssNr_PresentBoundary.h`.
- `retoque/dlssnr/DlssNr_Dx11Guides.cpp`, `DlssNr_PresentHost.cpp`, `DlssNrFeature_Vk.cpp`, `DlssNr_VkPresentHost.cpp`.

## 2. Information that arrives, and what NR uses

| Information | Availability and current use | Consequence |
|---|---|---|
| Color, output, depth, motion | Supplied through supported evaluate adapters; NR prepares owned color surfaces and readable guides | These are already the foundation; a new capture framework is not the first step |
| Render/display sizes, independent guide bases | Gathered from current evaluate/feature geometry; not simply inferred from allocation size | Preserve rect origin and valid extent through every conversion and resize |
| MV scale, low-resolution flag, inverted depth | Included in the guide frame and used by guide preparation | Keep units and resolution reference explicit; sign and axis conventions differ between APIs |
| Exposure texture and pre-exposure | Used during color preparation; not independent arguments sent straight to the NR network | Display-referred Present intentionally does not inherit scene exposure as if it were scene-linear color |
| Reset | Game reset plus local reset policy; `ResetIsPolicy` distinguishes one fallback cause | A camera cut and a temporary lack of guides must remain different events |
| Jitter and MV-jitter convention | Upstream adapters can supply them; replay records jitter, but `DlssNrFrameInfo` and the model call do not currently transport it | Preserve/diagnose it first; lack of a forwarded value is not by itself proof of a model bug, especially after an upscaler has resolved jitter |
| Camera, engine frame ID, viewport | Available in some SDK contracts; absent from the NR frame structure | Existing present epochs are useful observations, not authenticated engine frame IDs |
| Reactive / transparency / responsive masks | FSR/XeSS adapters translate some into the NGX block; NR does not consume them as those masks | Each mask needs its own meaning and an actual consumer; do not reinterpret it as NR ControlMask or UI alpha |
| HUD-less color and UI tags | Streamline hooks already receive these in supported FG configurations | An NR consumer needs independent frame/view matching and resource-lifetime handling |

`hkslSetConstants` currently forwards values and the frame token to `slFGInputs`; the viewport passed into the hook is not part of that forwarding call. Tag capture is also tied to installed FG hooks. This is an internal integration opportunity where a game already supplies the data, not evidence that all games expose it.

Further source anchors: `DlssNr_Dx12_Inputs.cpp::GatherGuideFrame/GatherFrame`, `DlssNr_Common.h::DlssNrFrameInfo`, `DlssNr_RenderSize.h`, `DlssNr_GuideMatch.h::ModelMotionScale`, `OptiScaler/hooks/Streamline_Hooks.cpp::hkslSetTagForFrame/hkslSetConstants`, and `OptiScaler/inputs/FG/Streamline_Inputs.cpp::setConstants/GetResourceType`.

The XeSS D3D12 adapter writes `DLSS.Exposure.Scale`, while the NR gather/preparation audited here does not read that scalar. Treat this as a contract question requiring a controlled image comparison, not an automatic instruction to multiply exposure again.

## 3. A focused input-contract extension

This is a proposal, not an implemented interface.

| Part | Preserve when actually available | Use |
|---|---|---|
| Provenance | API/provider, evaluate stage, real/estimated/zero guide class | Explain the effective route and prevent a synthetic source being labeled as engine geometry |
| Identity | Feature/context generation, optional engine frame token and viewport, capture sequence, observed present boundary | Match inputs from one rendered view; retain current conservative fallback when IDs are absent |
| Geometry | Valid source/destination rects, allocation shape, render/display references | Avoid allocation-size and subrect assumptions |
| Motion/depth meaning | Units, direction, jitter inclusion, dilation/coverage flags, depth convention when known | Normalize once at a defined boundary; keep unknown different from zero/false |
| Color meaning | Scene/display reference, transfer encoding, primaries, exposure/pre-exposure provenance | Prevent repeated tonemapping or treating encoded values as linear light |
| Resource use | Resource/view generation, content-validity interval, producer submission, consumer queue and retirement | Decide whether to borrow, copy, decline or use an explicit fallback |

A CPU snapshot freezes metadata and references; it does not freeze GPU texture contents. `AddRef`, pointer equality and equal dimensions are not content-lifetime proofs. A borrowed resource requires correct producer/consumer ordering and a guarantee that the game will not overwrite or alias it before consumption.

Streamline describes separate validity intervals and frame-based tagging [R1]. XeSS-FG likewise exposes resource validity, frame IDs and valid regions [R4]. These are useful integration patterns, not drop-in contracts for a private NR ABI.

Start by observing existing SDK data only while NR needs it. A bounded per-frame/per-view record can report available/used/rejected fields without copying every texture. Extend consumers one at a time. Multiple views currently trigger NR stand-down; recording view IDs alone does not implement independent per-view model histories.

## 4. Present: correctness and measurement before new transport

The current ring owns copies of depth and motion. Selection checks submission eligibility plus device, output extent/base, swapchain, ambiguity, consumption, age and render subrect. The strict epoch-lag check is enforced at the game's observed Present boundary, not universally at lower wrapped entries.

This classification helps avoid processing generated frames as fresh engine frames. It is still boundary evidence. It must not be described as universally knowing the engine's frame ID. A refused capture must keep its specific reason; fallback/stand-down policy must not silently describe reused or synthetic guides as real.

The frame can have matching dimensions and still be misaligned with the guides after lens distortion, depth-of-field, HUD or other post-processing. DLSS-G's HUD-less contract explicitly concerns the final scene with matching post-processing/color interpretation, rather than any earlier upscaler output [R3].

### First implementation candidate: complete route-cost accounting

Reuse the existing fence-confirmed timing machinery and diagnostic export. Capture:

- The Present command list's GPU interval, including copy-in, synthesized motion when enabled, HDR conversion, NR/composition and copy-out.
- Guide-capture copy cost on its actual producing evaluate list.
- Bridge conversion cost for D3D11, and equivalent intervals for Vulkan only where that host can prove query completion.
- CPU preparation, submission and any CPU wait as separate measurements.
- Capture age and boundary lag, together with accepted/rejected source, sizes, model/config identity and initialization/steady-state classification.

Do not sum nested GPU intervals. Do not subtract timestamps from unrelated queue clocks without calibration. Do not add overlapping work across queues and call the result frame latency. Do not introduce a per-frame CPU wait merely to read timing results. Report route GPU work, CPU blocking and observed frame-time impact separately.

The existing model/Dispatch timing cannot, on its own, account for work performed outside that span. The diagnostic export already separates detected from used inputs; extend it instead of adding a competing report.

### Later Present experiments

1. Observe which HUD-less/UI tags the selected game actually provides, including frame/view identity and validity.
2. Diagnose guide/image agreement, initially without changing routing. A sparse reprojection score is a heuristic, affected by disocclusion, lighting and post-processing; it is not proof that a particular guide is wrong.
3. Add a HUD-aware route only for a verified input contract. Exact layer recomposition needs the game's blending and color semantics. UI-alpha masking of an already composited image does not automatically reconstruct a correct semitransparent UI layer.
4. Consider real guides for native Vulkan Present only after a title demonstrates value and the Vulkan producer/consumer lifecycle is designed.
5. Consider removing a backbuffer copy only after measuring it and proving destination format, usage flags, state and all downstream reads. A raster rewrite is not automatically cheaper than a copy.

## 5. Upscale/evaluate: concrete performance candidates

All rows below are proposals found by code inspection. None has a new GPU or FPS measurement.

| Candidate | Applies to | Work it could remove | Required evidence |
|---|---|---|---|
| One metadata snapshot per evaluate boundary | D3D12 evaluate preparation | Repeated parameter Gets, resource descriptions and diagnostic geometry queries | Separate pre/post-backend snapshots; the backend may change parameters. No global cache keyed only by parameter pointer |
| Reuse original color instead of writing a second original-image snapshot | Eligible pre-SR path | A render-size store and associated transitions | Source remains unchanged; compatible format/extent/subrect; correct Hold/capture/pulse/metrics consumers and failure restoration |
| Share identical encode outputs | Eligible SDR UNORM passthrough | One full-surface store | Explicit format list and consumer audit; float/scRGB negative values make this invalid in the general case |
| Sample a compatible typeless resource through a typed SRV when GuideMatch will run | Guide-resampling path | The preliminary full-resource clone/copy | Source is sampleable; correct view format/aspect/mips/state; retain fallback for unsupported or denied SRVs |
| Extend existing descriptor-generation caching to GuideMatch | D3D12 guide matching | Four view creations on unchanged compatible bindings | Identity/generation and complete view arguments; invalidation on resource/context recreation; preserve descriptor-slot lifetime |

Anchors: `EvaluateAfterUpscale` around frame gathering; `ScopedPreUpscale` around its private-output dispatch; `dlssnr.hlsl` encode mode 0; `DlssNr_Dx12_Resources.cpp::ReadableGuide`; `DlssNr_GuideMatch_Dx12.cpp::Dispatch`; and the existing view cache in `DlssNr_Dx12.cpp::DispatchPass`.

Persistent upload mappings and cached composition descriptors already exist. Parameter-slot layout has already been audited. Removing NGX Sets because a value appears unchanged is unsafe where a parameter block is shared with the game's DLSS. Rewriting those overload slots offers no demonstrated gain.

NVIDIA's descriptor guidance supports avoiding unnecessary descriptor creation [R7]. Its synchronization guidance calls for measuring actual overlap and avoiding redundant waits [R8]. Neither source justifies changing a resource lifetime or adding async compute without proving benefit on this route.

For the RTX 4090, keep the current full-quality post-RR scale as the reference. Lower resolution or reduced model cadence is a quality/cost policy change; it should not be mixed into evidence for an equivalent-output optimization. Most GPU-copy candidates above have limited reach in HDR RR at 1.0.

## 6. Modes with distinct purposes

Separate placement, input source and quality policy. Existing `Stage`, `HookMethod`, RR settings, synthesized motion and working scale already cover parts of this; avoid duplicating them with competing toggles.

| User-facing purpose | Status | Meaning |
|---|---|---|
| After upscale | Existing | Model on reconstructed output with evaluate guides |
| After ray reconstruction | Existing | Denoised input with the RR-specific scale/pass policy |
| Before upscale | Existing experimental | Model at render resolution; assess its interaction with jitter and the subsequent upscaler |
| Present with engine guides | Existing on eligible D3D12/bridge routes | Final frame plus accepted captured guides; show any refusal/fallback |
| Present compatibility | Existing variants | Image-only/zero guides or estimated motion, with accurate provenance |
| Present with engine HUD data | Proposed | Verified final-stage HUD-less/UI inputs and explicit composition contract |
| Per-object/material NR control | Research candidate | Engine-provided control mask, after runtime-specific effectiveness is established |

A quality policy such as full resolution or a measured budget belongs alongside these placements. It is not another input source. `ApplyModel=false` is an image-comparison control that can retain model work; `Enabled=false` disables the route. `Passthrough` means an already-tonemapped color path, not NR disabled.

## 7. New NR reference versus the local ControlMask evidence

The public `nvpro-samples/vk_gltf_renderer` documentation describes an NR control image with separate channels for intensity, local tone, local structure and global tone, using RGBA16F in that example and disabling AutoMask when supplied [R5]. The header labels the NR integration as requiring a beta NGX SDK [R6]. This is evidence of the example's intended contract, not a guarantee about the installed private model ABI.

Local evidence is more cautious:

- `tests/dlssnr-loopback/control-mask.md` records thirteen older trials without an observable mask effect, while the independent automatic-mask control did affect output.
- The README records a newer 24-trial per-channel retest as written but not yet run. It uses RGBA8 and fixes alpha at 255.
- Therefore it does not yet validate the sample's global-tone alpha-channel claim or its RGBA16F usage.

A targeted follow-up should compare a null mask with all ones, then vary one channel while the others remain one, using the exact runtime/model identity. Add a global-tone positive control and a format comparison if the new sample contract is pursued. Preserve repeatability and the null-after-mask case because the parameter block can retain keys.

A mask controlling where the edit appears does not establish that the model skips computation in those pixels. Do not advertise it as a GPU optimization. Reactive masks, UI alpha and NR artistic-control channels are different inputs.

## 8. What needs engine cooperation

Where SDK interception already supplies a field, improve internal transport first. A game can provide depth/motion to FG even when SR is disabled; that is a more explicit source to investigate than universal depth-buffer discovery.

Where the game supplies nothing, an engine adapter/plugin can expose the chosen post-processing stage, material/object control masks, correct moving-object vectors, view identity, UI layers and explicit discontinuities. Those are capabilities of an integration, not data that a Present hook can infer reliably from final RGB.

Reprojection from depth and camera matrices accounts for camera motion on static geometry; it does not recover arbitrary animated-object motion [B3]. Optical flow is an estimate with different failure cases. The local design `present-depth-without-upscaler.md` already parks universal heuristic capture behind a value experiment. Reuse that investigation instead of installing broad draw/barrier hooks first.

## 9. Books and chapters actually consulted

These were specific public chapters, not cover-to-cover readings. Older GPU examples supply principles; current APIs and local measurements decide the implementation.

| Reading | Useful principle for this project |
|---|---|
| GPU Gems, chapter 28, Graphics Pipeline Performance [B1] | Locate the limiting stage and measure the whole pipeline; optimizing an unrelated pass may not change frame time |
| GPU Gems 2, chapter 35, GPU Program Optimization [B2] | Separate computation, memory traffic and asynchronous CPU/GPU effects |
| GPU Gems 3, chapter 27, Motion Blur as a Post-Processing Effect [B3] | Understand camera reprojection and its limits for moving objects |
| GPU Gems 3, chapter 24, The Importance of Being Linear [B4] | Filtering/composition must respect color encoding; numerical format alone is insufficient |
| Physically Based Rendering, fourth edition, section 8.8, Image Reconstruction [B5] | Reconstruction filters trade blur, aliasing and ringing; guide/color resampling needs image evidence |

Microsoft's MiniEngine temporal code [R9] provides a concrete example of engine-owned jitter, history reset, previous/current depth and velocity. AMD's upscaler header and manual [R10, R11] show the richer dispatch metadata and distinct transparency/reactive semantics. They inform the contract; they do not prove those buffers are accepted by NR.

## 10. Recommended work order and validation

1. Complete per-route cost and effective-input diagnostics using existing timing/reporting infrastructure.
2. Preserve SDK frame/view/provenance metadata in a bounded NR-specific record; initially observe availability and rejection without changing the image.
3. Implement one measured, applicable equivalent-output optimization at a time. CPU snapshot and GuideMatch descriptor caching are narrower changes than copying fewer textures across an uncertain lifetime.
4. Validate HUD-aware Present for one selected title with verified tags. Keep final-stage color and generated-frame handling explicit.
5. Revisit the already-written ControlMask experiment against the new example's semantics and exact model version before any material-control UI.
6. Pursue an engine adapter or Vulkan real-guide transport only when a concrete title/input gap establishes the value.

For GPU changes, compare the same scene, model, scale, guides, output format and FG state. Separate initialization and steady state, record median and tail frame times, and use paired frames/replay plus temporal scenes (camera movement, moving objects, HUD, particles, cuts, resize/DRS) as relevant. Existing host suites verify contracts; they do not prove GPU output equivalence. The repository also requires native-Windows evidence for new D3D12 passes; Proton results establish the tested Proton path only.

No new tests were necessary for this read-only study. The next code change must select the existing suites and runtime checks that address its actual risk, rather than repeating every test indiscriminately.

## 11. Documentation drift found

`retoque/dlssnr/README.md` still calls present identity log-only, while the current code enforces it. `GatherFrame` retains a comment describing exposure as diagnostic-only although preparation consumes it. Older sections of the Present/Vulkan/HUD designs predate later implementations. Use the current source and dated follow-up evidence when resolving these conflicts; this note does not rewrite those historical records.

## Primary references

- [R1 — Streamline resource tagging and lifecycle](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md)
- [R2 — Streamline common constants](https://github.com/NVIDIA-RTX/Streamline/blob/main/include/sl_consts.h)
- [R3 — Streamline DLSS-G integration](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md)
- [R4 — Intel XeSS-FG developer guide](https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md)
- [R5 — nvpro glTF renderer NR documentation](https://github.com/nvpro-samples/vk_gltf_renderer/blob/main/docs/denoising.md)
- [R6 — nvpro glTF renderer DLSS header](https://github.com/nvpro-samples/vk_gltf_renderer/blob/main/src/dlss.hpp)
- [R7 — NVIDIA Advanced API Performance: Descriptors](https://developer.nvidia.com/blog/advanced-api-performance-descriptors/)
- [R8 — NVIDIA Advanced API Performance: Synchronization](https://developer.nvidia.com/blog/advanced-api-performance-synchronization/)
- [R9 — Microsoft MiniEngine TemporalEffects](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/MiniEngine/Core/TemporalEffects.cpp)
- [R10 — AMD FSR upscaler dispatch header](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/upscalers/include/ffx_upscale.h)
- [R11 — AMD FSR super-resolution integration](https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-upscaler/)
- [R12 — Microsoft IDXGISwapChain::Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)
- [R13 — Streamline DLSS-SR integration](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md)
- [B1 — GPU Gems chapter 28](https://developer.nvidia.com/gpugems/gpugems/part-v-performance-and-practicalities/chapter-28-graphics-pipeline-performance)
- [B2 — GPU Gems 2 chapter 35](https://developer.nvidia.com/gpugems/gpugems2/part-iv-general-purpose-computation-gpus-primer/chapter-35-gpu-program-optimization)
- [B3 — GPU Gems 3 chapter 27](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-27-motion-blur-post-processing-effect)
- [B4 — GPU Gems 3 chapter 24](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-24-importance-being-linear)
- [B5 — PBRT fourth edition, Image Reconstruction](https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Image_Reconstruction)

External main/master pages were consulted on 2026-11-01. They describe the retrieved upstream state, not necessarily the dependency versions vendored by this checkout. Sample documentation was distinguished from a complete binary ABI contract; unavailable source bodies were not treated as inspected implementation.
