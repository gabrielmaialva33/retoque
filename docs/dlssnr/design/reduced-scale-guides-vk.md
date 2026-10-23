# Guides at the working size on the Vulkan route

Status: **built 2026-10-23; host-tested, and run on a real device under Wine (`tests/guide-match-vk`,
`tests/vulkan-present-host` `nr-route-guides`); not measured in a game.** The Vulkan half of
[reduced-scale-guides.md](reduced-scale-guides.md) (fix 1, matched guides), which holds for this route unchanged except
where this note says otherwise. Written before the code, as the design; the sections below are what was built.

## What is wrong now

`EvaluateAfterUpscaleVk` (`retoque/dlssnr/DlssNrFeature_Vk.cpp`), below `WorkingScale=1`:

- the colour is box-downsampled to the working size (`proxySmall`), and the model is built for that size;
- depth and motion go to the model as the game's own regions, at the frame's guide size (`guideWidth`, `motionWidth`
  and their bases);
- the motion-vector scale is already fix 2's (`GuideMatch::ModelMotionScale` against the game's region), and the code
  says once that fix 1 is missing: `Vulkan guides: larger than the WxH model and not resampled (no Vulkan resample
  yet)`.

That is fault 1 of the D3D12 note, the one jlrouzies-fr found: each working-size pixel is handed the depth and motion
of some other place. On D3D12 it was a visible scintillation, which went away with matched guides (Cyberpunk 2077 at
0.5, 2026-09-28). On Vulkan it is the suspected cause of shimmer at reduced scale. That is an inference from the D3D12
A/B, not a Vulkan measurement.

**Who it hits.** Every after-upscale Vulkan run with a guide larger than the model:

- RTX 20 and 30 on `WorkingScale=auto`, which resolves to 0.5 there ([working-scale-auto.md](working-scale-auto.md));
- anyone who moves the model-resolution slider below the render scale. DOOM Eternal at DLSS Quality, 3440x1440, has
  2293x960 guides; at 0.5 the model is 1720x720.

**Not affected:** the Vulkan present host. Its guides are made at the working size: zero guides at build time, and the
synthesized field is estimated on the model's own input (`nr-vulkan-present-host.md`, stage 5).

## What changes

The D3D12 rules, applied to this route as they stand in `DlssNr_GuideMatch.h`:

1. **When.** Below native (the working size under the frame's on some axis), and `GuideMatch::Wanted`: `[DlssNr]
   MatchGuides` on (the default), and either guide larger than the model's input along some axis.
   - A guide smaller than the model (supersampling, or render size under the working size) is the upscaler's ordinary
     subrect contract and is handed over as before.
   - **Where this differs from D3D12.** D3D12 also resamples at or above native when a guide is larger than the frame,
     which happens when a game publishes no render size over a depth allocation larger than its output. Here that case
     keeps the route's behaviour from before. A refusal at native would stop NR where it used to run, and the case is
     the render-size fallback gap, not this one. (Found by review on 2026-10-23, after the first version had no
     below-native gate.)
   - **The geometry** (V-04 and V-05, Codex's review). The resample turns whatever region it is given into a whole
     working-size guide, so the region must be the picture:
     - below native, a render geometry the resolver reports partial (`Incomplete`) or retired (`Stale`) is refused,
       not replaced by the whole allocation, whose margin a dynamic-resolution game may never have written. A frame
       with no geometry at all (`Missing`) keeps the allocation, as D3D12 does; at native nothing changes;
     - the low-resolution vectors' region and their scale are measured against the render size when it is known, kept
       apart from the depth's region, which its resource and subrect base clip, as the D3D12 consumer does. That fix is
       `242f1a05` (the metamorphic geometry work, `tests/nr-geometry-metamorphic`), taken here as it is. Before it, a
       depth clipped to 1024 of a 1280 render made the model read 1024 texels of motion at `960 / 1024` instead of
       1280 at `960 / 1280`.
2. **What.** Depth and motion are point-resampled from the game's regions into two working-size images. Those are handed
   to the model as a full zero-origin region at the working size.
3. **Filter.** Point, the same `PointSource` mapping as D3D12. The D3D12 note explains why: a depth between two
   surfaces is a surface that does not exist, and a vector averaged across an edge belongs to neither side. It is the
   same HLSL file, `precompile/dlssnr_guides.hlsl`, compiled to SPIR-V with `-spirv -D VK_MODE`. Everything the
   Vulkan build adds is under `VK_MODE`, so the DXIL is unchanged (recompiled and compared before committing).
4. **Scale.** Fix 2 as on D3D12. With `RenderMotionScale` on, the game's scale times the handed motion extent (the
   working size when matched) over the reference: the render size for low-resolution vectors, the frame's otherwise.
   Off, the legacy working / frame conversion.
5. **Formats.** `R32_SFLOAT` depth and `R32G32_SFLOAT` motion, D3D12's `R32_FLOAT` and `R32G32_FLOAT`. Vectors in
   pixels reach the thousands, where 16-bit float has lost the sub-pixel, and a 16-bit source copies into them exactly.
   - DLSS's own contract accepts both: section 3.3 of NVIDIA's DLSS programming guide says motion vectors in RG32 or
     RG16 float, and depth in any single-channel format.
   - The D3D12 model has read these formats under vkd3d-proton since 2026-09-28, which is the same Vulkan model
     underneath.

### The shader's Vulkan interface

On D3D12 the trap was a 48-byte constant-buffer view, which vkd3d-proton accepted and native Windows answered by
removing the device. The Vulkan equivalents are checked against the specification, not against what runs here.

**Constants.** Push constants: one block of 12 `uint` (48 bytes), one range, compute stage, offset 0.
- `maxPushConstantsSize` is guaranteed to be at least 128.
- A range's offset and size must be multiples of 4, the size nonzero, and offset plus size within that limit.
- The values are recorded into the command buffer (`vkCmdPushConstants`). So there is no buffer, no ring of slots and
  no offset alignment, and no host write can overwrite constants a pending dispatch has not read yet.
- `nr-invariants` holds the C++ block equal, field for field, to the shader's `GuideParams`, at most 128 bytes and a
  multiple of 4.

**Descriptors.** One set with four bindings and no sampler, because every read is an `OpImageFetch`:

| Binding | Type | What |
|---|---|---|
| 0 | `SAMPLED_IMAGE` | the game's depth view |
| 1 | `SAMPLED_IMAGE` | the game's motion view |
| 2 | `STORAGE_IMAGE` | the working-size depth (`r32f`) |
| 3 | `STORAGE_IMAGE` | the working-size motion (`rg32f`) |

- **The descriptors are pushed into the command buffer** (`vkCmdPushDescriptorSetKHR`, a set layout made with
  `VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR`). The pass owns no set and no pool. What a recording binds is
  in the recording, however many evaluates one command buffer holds and however often a game submits a kept buffer
  again. Four descriptors, under the 32 the extension guarantees (`maxPushDescriptors`).
- **Why not sets (V-01, Codex's review, 2026-10-23).** The first version had a ring of 16 sets rewritten at each
  dispatch without a fence. Updating a set that a recording still names invalidates that recording
  (`VUID-vkQueueSubmit-pCommandBuffers-00070` at its submit), or breaks `VUID-vkUpdateDescriptorSets-None-03047` if it
  is pending. A seventeenth evaluate in one command buffer, or a buffer kept executable and submitted again after
  sixteen others, did exactly that: `tests/guide-match-vk` `one_buffer_x20` and `resubmit_after_17` failed on it with
  `commandBuffer-recording` and `pCommandBuffers-00070` errors. No ring size depends on how long a game keeps a
  recording; push descriptors take the question away.
- **`VK_KHR_push_descriptor` must be enabled on the device.** It is one of the model's device extensions
  (`DlssNr_VkExtensions.h`); OptiScaler's device hook and the Vulkan present host add it wherever the device offers it
  (and not beside a Vulkan 1.4 `pushDescriptor` feature left off). The pass asks `vkGetDeviceProcAddr` for
  `vkCmdPushDescriptorSetKHR`, which answers NULL for a command of an extension that is not enabled. Without it the
  pass is not made, and that is a stable refusal on the device.
- **The storage formats are mandatory.** `rg32f` needs the SPIR-V `StorageImageExtendedFormats` capability. Vulkan
  allows that capability without a device feature. The spec's note on `shaderStorageImageExtendedFormats` says that
  feature "only adds a guarantee of format support", and that `R32G32_SFLOAT` is covered by mandatory format support
  anyway. `R32_SFLOAT` storage is mandatory too.
- The format support is still queried once per device, not assumed: `STORAGE_IMAGE` and `SAMPLED_IMAGE` in
  `optimalTilingFeatures`, and `vkGetPhysicalDeviceImageFormatProperties` for the exact usage the route creates them
  with, at the working size.

**The game's images.** They are bound as NGX was handed them, in `SHADER_READ_ONLY_OPTIMAL`, and never transitioned.
That is the DLSS contract: section 3.4 of the programming guide says inputs are in the read state ("Sample Image" on
Vulkan, with `SAMPLED` usage), and DLSS always returns them to it. This tree already relies on it in `DT_Vk`, `VC_Vk`,
the Vulkan-on-D3D12 bridge and the route harness. A view is refused, never bound, when the shader would read it
invalidly:

- **A depth view that names stencil.** A sampled view of a depth/stencil image must name one aspect
  (`VUID-VkDescriptorImageInfo-imageView-01976`), and stencil is an integer the shader would read as float.
- **An integer format** (`*_UINT`, `*_SINT`). The view's numeric type must match the shader's float sampled type
  (`VUID-vkCmdDispatch-format-07753`).
- **Motion with one component**, which has no y to read.
- **More than one layer.** The shader's image is 2D, not arrayed, and the view's type must match it
  (`VUID-vkCmdDispatch-viewType-07752`). NGX's resource struct carries no view type, so this catches only a layer count
  above one. A `2D_ARRAY` view of a single layer cannot be told apart from a `2D` one, and is read as the model reads
  it.
- **An empty region.** A game's subrect base at the edge of its image leaves a region of no texels once the route has
  clamped it, and the shader's `regionExtent - 1` would wrap and read past the image. D3D12's `Dispatch` refuses the
  same.
- **A format without `SAMPLED_IMAGE`** in the device's optimal-tiling features.

The views the shader takes are the same ones the upscaler and the model already read.

**Barriers.**

- **Before the dispatch:** both outputs go from `UNDEFINED` to `GENERAL`, with first scope `ALL_COMMANDS` and
  `SHADER_READ | SHADER_WRITE`.
  - `UNDEFINED` discards their contents, which is safe because the dispatch writes every texel.
  - The first scope orders the previous frame's model read (write after read) and resample write before this write.
  - Storage images must be in `GENERAL` (`VUID-VkWriteDescriptorSet-descriptorType-04152`).
- **After the dispatch:** `COMPUTE_SHADER` and `SHADER_WRITE` to `ALL_COMMANDS` and `SHADER_READ`. The model's kernels
  see the writes whatever stage they run in.
- **The game's images need no barrier.**
  - They are read after the game's own barrier, which made them readable for the upscaler earlier in the same command
    stream. A read after a read needs none.
  - Write after read against the game's next write is the game's barrier's job, and it already covers the upscaler's
    reads and the model's, recorded before it.
- The model reads the outputs in `GENERAL`, as it reads this route's colour input and the present host's working-size
  guides.

**Dispatch.** The group is 8x8x1, 64 invocations (Vulkan guarantees 128), and the dispatch is `ceil(work / 8)` groups
per axis.

### Where it runs

On the game's command buffer inside the route, after the encode and the downsample, immediately before the model.

The decision comes before the frame's own commands. Whether the resample is wanted, and whether it can run (the pass
made, the images there, the game's views readable), is settled after the guide geometry is known and before the first
timestamp, barrier or dispatch. A refusal therefore records none of the pass's commands. The one thing that can precede
it is a feature the route built that frame after a resize, which is NGX's own creation and is kept for the next frame.
Recording itself cannot fail once that is settled, because descriptor writes and `vkCmd*` return nothing.

### Lifetime

This follows the README's "Vulkan lifetime" and `DEVELOPMENT.md` invariant 8.

**What a drain does not prove (V-02, Codex's review).** `vkDeviceWaitIdle` and the hand-back's fences cover work that
was submitted. They say nothing of an evaluate the game recorded and has not submitted yet, or of a command buffer it
keeps and submits again, and either can still name these objects. Nothing the route sees tells it when the game resets
or frees its command buffers. So the guide resample's objects are never destroyed at a drain; they are retired:

- **The pass** (pipeline and its two layouts) is made once per device, at the first frame that wants it.
  - In `FreeRouteObjects` (a hand-back after its drained fences, a device going with its work seen complete,
    `ShutdownVk(true)`) it is copied into a `DeferredRelease` entry owned by its device (`misc/DeferredRelease.h`, the
    module's retirement, which `Shader_Vk` and so the route's composition pass already use) and forgotten.
  - It is abandoned in `ShutdownVk(false)`: handles forgotten, nothing called.
  - It is a plain handle bundle: no destructor touches Vulkan.
- **The two images** are created at the working size the first time they are wanted.
  - A resize keeps the pair of the old size alive (`depthRetired`, `motionRetired`) until the next resize, which hands
    it to `DeferredRelease`. An evaluate recorded at the old size and submitted after the resize still finds its
    guides.
  - `FreeRouteObjects` retires the current and the kept pair the same way; `ShutdownVk(false)` abandons both.
- **`DeferredRelease`** runs an entry after 8 presents and 500 ms, or after 2 s where presents are not hooked. The
  Vulkan present host finishes a device's entries (destroying after a drain, dropping after a failed one) before
  `vkDestroyDevice`, and a new device drops the entries of the old.
- **The drain's own result now decides.** In the resize block: `VK_ERROR_DEVICE_LOST` abandons everything
  (`ShutdownVk(false)`) and ends the route's session; any other failure frees nothing and the resize is tried again at
  the next evaluate. `ShutdownVk(true)` abandons after a failed drain. The filter rebuild for supersampling waits for a
  drain that succeeded. `PrepareGuideMatch` passes `lost` to `CreateImageOn`, so a device lost during the allocation
  abandons the half-made pair instead of destroying it.
- **The residual window, stated plainly.** A recording made two resizes ago, or one `DeferredRelease` would have
  retired (older than 8 presents and 500 ms, or 2 s), and still submitted afterwards, would find its guides gone. That
  is the same bound the module puts on every `Shader_Vk` object. The rest of the route's objects (its feature, proxy,
  output and the composition pass's descriptor ring) are not changed here: they are still freed at the resize block's
  drain and in `FreeRouteObjects`, the window Codex's review calls inherited.

### When it cannot run

**With `MatchGuides` on, the model is never handed guides that do not match its colour.**

When the resample is wanted and cannot run, the route does not run the model on that frame:

- it returns before recording anything, so the frame stays as the upscaler wrote it;
- it sets the history reset for the next frame it does run;
- it says why once per reason, naming the way out: `WorkingScale=1`, or `MatchGuides=false`.

The reasons that can apply:

- the output formats are not supported at the working size, or the pass could not be made, or its images could not be
  allocated;
- the game's view is one the shader cannot read, or a region is empty (above).

They are latched differently (V-03, Codex's review: the first version cached an allocation failure until a resize):

- **Stable, until the device or the frames change:** a device without push descriptors, a lost device, outputs the
  device cannot hold at the working size.
- **Transient, tried again after a backoff:** the pass or its images not made for want of memory. The first retry is
  after 1 s, each failure in a row doubles the wait, up to a minute. No resize is needed.
- **The frame's own** (a view the shader cannot read, an empty region, partial or stale geometry): looked at again on
  the same doubling schedule.

**A refusing route stops holding the model.** The decision comes after the route has asked for the model
(`ClaimModelForAfterUpscale`), because the pass needs the route's device. The first version therefore kept the model
while it refused every frame, and with the Vulkan present host on, neither side ran NR.

- Now a refused frame is recorded by what is known before the ask: the device, the frame and working sizes, and the
  game's guide images and their formats.
- Every later evaluate of that shape returns before asking, while the refusal stands: stable ones until the shape
  changes or `MatchGuides` goes off, the others until their retry is due.
- The route's hold then lapses, and after `kAfterUpscaleIdleMs` (10 s) the present host takes the model back through
  the ordinary hand-back, which drains and frees the route's objects as before.
- A retry that falls due asks again; the present host stands down for it, as it does for any evaluate. Because the
  waits double, a condition that lasts gives the present host the model for most of the time, not a ping-pong every
  few frames.
- The record is the frames', not the device objects', so it survives the hand-back.

The menu's frame count stops while frames are refused; the log line is the explanation.

Rejected alternatives:

- **Frame-size guides,** which is D3D12's fallback when its resample does not run. That is the very fault this fixes,
  and on this route it would come back silently.
- **Running at 1.0.** That means a feature rebuild at about four times the model's cost on the GPUs that chose 0.5
  because 1.0 is too slow, and a working-scale change made behind the user's back.

Every one of these reasons is theoretical on the hardware the model runs on: the output formats are mandatory, and NGX
already reads the game's views. The refusal exists so that a surprise costs one frame of NR, not a mismatched history.

**`MatchGuides=false` keeps its D3D12 meaning**: the A/B arm, frame-size guides, which is this route's behaviour until
now. It is said once in the log.

### Default-identical

The new path records nothing and creates nothing in these cases:

- `[DlssNr]` off;
- at `WorkingScale` 1 or above, by the below-native gate, whatever the guides' sizes;
- when no guide is larger than the model.

The route then takes exactly the path it took before, with the same evaluate arguments. Of the log lines, only the old
`not resampled (no Vulkan resample yet)` is gone.

## What the log says

Once per change, the D3D12 lines with the route's prefix. These are DOOM Eternal's numbers at 0.5 and DLSS Quality:

```
DLSS-NR Vulkan guides matched to the working size: depth and motion 1720x720 for a 1720x720 model (the frame's guides are 2293x960 and 2293x960)
DLSS-NR Vulkan model motion scale 1720.0 x 720.0: game scale 2293.0 x 960.0 measured against 2293x960 (render size, low-resolution vectors), motion texture 1720x720 (matched to the working size), model 1720x720
```

The pass's creation, once per device and again after each hand-back freed it:

```
DLSS-NR Vulkan: the guide resample is up (push constants 48 bytes, 4 push descriptors)
```

With `MatchGuides=false` and guides larger than the model, once: `DLSS-NR Vulkan guides: larger than the 1720x720 model
and not resampled (MatchGuides is off); the model gets the frame-size guides`.

A refusal reads, once per reason:

```
DLSS-NR Vulkan guides: larger than the 1720x720 model and the resample cannot run (<reason>); the model is not run on these frames rather than handed frame-size guides (WorkingScale=1 or MatchGuides=false runs it)
```

## Tests

- **Host, a new suite `nr-vk-guides`, red before the code.** It holds three things:
  - what the forwarder is handed (sizes, bases, scale) for DOOM's numbers, DLSS Performance, supersampling,
    `MatchGuides` and `RenderMotionScale` off;
  - the refusal rule and the view readability rules;
  - the push block, through source guards on the route: the decision before any recording, the evaluate's arguments
    from the plan, frees only after a drain, abandon in `ShutdownVk(false)`, nothing new at scale 1.
  - It drives fakes and pure functions, not Vulkan.
- **Wine, a new suite `guide-match-vk`.** The production pass on a real device under Wine, with the Khronos layer's
  core and synchronization checks (proved active) and zero errors required.
  - Analytic depth goes in through `D32_SFLOAT`, a depth view of `D32_SFLOAT_S8_UINT` and an `R32_SFLOAT` colour image,
    and analytic motion through `R16G16_SFLOAT` and `R32G32_SFLOAT`, all in `SHADER_READ_ONLY_OPTIMAL`, with non-zero
    subrect bases.
  - The shapes: DOOM's 2293x960 under 1720x720, display-resolution vectors, the route harness's 960x540 under 640x360,
    an exact 2:1, and an extent that is not a multiple of eight. DOOM's shape also runs 20 frames with three in flight,
    the inputs changing and the outputs shared.
  - V-01's two cases: 20 evaluates recorded into one command buffer before its single submit, and a buffer kept
    executable, submitted, then submitted again unchanged after 17 other evaluates ran.
  - Every texel read back must equal `PointSource`'s prediction exactly.
- **Wine, `vulkan-present-host`: `nr-route-guides`,** `nr-route-quiet` with the game rendering 960x540 under
  `WorkingScale=0.5`, behind OptiScaler's FSR 2.2. The route's 640x360 model has larger guides. It holds:
  - the matched line, the scale line and the pass made on each of the route's three runs;
  - no refusal, and every evaluate accepted by the real model reading the matched guides;
  - validation clean of the host's errors;
  - everything `nr-route-quiet` holds.
  The route's output is never presented, so the model's edit on it is not read back.
- **Not covered: the picture.** Whether the shimmer goes on DOOM Eternal at 0.5 is the in-game check: arm A the
  defaults, arm B `MatchGuides=false`, the same spot, a pan and a stop.

## Open after the second review (2026-10-23)

Codex re-read the fixes for its first review (V-01 to V-05) on the integration that took them. V-01 is closed for
this pass (push descriptors, no sets of its own) and V-05 for a known render size. Still open, and both older than
this pass:

- **V-02, P1: a generation's lifetime.** The resize drain's result is checked now and the guide pair outlives one
  resize through `DeferredRelease`, but that is a time margin, not a proof. An evaluate recorded and not yet
  submitted when a later evaluate resizes still references the old feature and surfaces, which the resize frees
  (`ShutdownVk`'s route objects, the proxy and native outputs); `DeferredRelease` frees the retained guides after
  presents and time, without knowing whether a kept command buffer is submitted again. Closing it needs the whole
  generation kept while any recording of it can still be submitted, or a refusal that holds the generation until
  a boundary that proves it.
- **V-06, P1: the composition pass's ring.** `DlssNr_Vk` cycles 18 descriptor sets and 18 slices of its mapped
  constant buffer with no tie to the command buffers that recorded them; with encode, reduction, meter and resolve
  (four dispatches) the 19th dispatch arrives in the fifth evaluate of one command buffer. Moving the sets to push
  descriptors is not enough on its own: the constants must be versioned with them (push constants, or a slice
  owned per recording).
- **V-03, V-04, P2.** Some transient failures are still classed as stable, and below native a `Missing` geometry
  still resamples the whole allocation (only `Incomplete` and `Stale` refuse).

None of these is exercised by today's GPU suites, which reset each command buffer and resize only after the GPU is
done; each needs a case with a recording kept across the event.
