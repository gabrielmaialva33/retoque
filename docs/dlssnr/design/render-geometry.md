# Render geometry: creation fallback and pre-upscale display motion

2026-10-23. Scope: D3D12, D3D11 and Vulkan NGX input evaluates (including their D3D12 bridges),
CPU geometry contracts only. No new setting, shader math or cbuffer change.

## Problem and decisions

A missing evaluate subrect/Width/Height currently becomes the guide allocation extent.
MSFS-shaped allocations can have invalid margins. ShyVortex 491e5af9/v0.9.71/72 falls
back through parameter dimensions to the current feature. We retain that priority but
never infer identity/freshness from the global currentFeature pointer: pre-upscale is
entered before the selected backend updates that pointer, and RenderWidth is mutable.

Record the successful upscaler creation's render/display dimensions by (input API, NGX handle),
using a snapshot taken before backend initialization can alter the creation parameters.
Release erases the record; a new successful creation at the same handle replaces it.
A scoped binding around the matching evaluate carries a value snapshot, restores the
previous binding on nesting/unwind, and is used only with that evaluate's parameter
block. Register/bind are gated on NR enabled; frame-generation evaluates do not bind.
Features created while NR is off have no fallback record if NR is later enabled.

Resolution priority:
1. A complete current nonzero render subrect.
2. Complete current Width/Height, including 1:1 rendering; OutWidth/Height, when known,
   bound these dimensions. Missing OutWidth is not a reason to discard valid render keys.
3. The matching feature's creation dimensions, only while the creation epoch is still
   compatible with every observed render/display extent.
4. Unknown/refused: never the whole allocation as guessed valid content.

A half-specified nonzero subrect is invalid, not permission to mix current and creation
axes. Incomplete/invalid Width/Height metadata also disarms the creation epoch: refusing
the malformed evaluate must not erase evidence and permit a stale fallback on the next one.
Once an explicit render size differs from creation (DRS), missing dimensions in
that epoch cannot fall back to creation again, even after the current size returns to
the original. A new creation re-arms the fallback. A display-size change also makes the
old creation fallback stale. Complete current metadata remains usable after either event.
No inference can detect an engine silently changing its valid area without metadata;
creation fallback relies on the feature contract until contradictory metadata arrives.
This is an explicit limit, not a freshness guarantee based on allocation size or time.

Frame gathering carries whether render geometry is known and its source. Unknown or
incomplete real-game guides are refused before recording NR. Zero/synthesized guides
remain governed by their existing non-game-guide contract.

### Follow-up: D3D11 and Vulkan bridge lifetime

The D3D11/Vulkan NGX inputs must register creation and bind evaluate too: their
IFeature_Dx11wDx12/IFeature_VkwDx12 bridges call the D3D12 NR path with that same parameter
block. A D3D12-input-only registry would now refuse valid bridge frames without dimensions.
Pack an explicit input API tag in the high 32 bits of a uint64_t key and the complete
32-bit NGX handle in the low bits. A single synchronized map then preserves the existing
record/scope lifecycle without cross-API collisions, erasure or DRS invalidation.

Snapshot before GetFeature/Init can normalize the parameters. Register only when the input
returns success with a published handle; D3D11's accepted handle with deferred backend
recovery also returns success and needs the same snapshot. Vulkan CreateFeature delegates
SR/RR to CreateFeature1, so registration belongs only in the latter. Forget on ReleaseFeature
also cleans records created while NR was on if it is off at release. Creation with NR off
only removes a previous epoch; it does not read dimensions or allocate a record.

Scope the entire input evaluate (including backend changes, bridge callbacks and native
post-upscale offers), using the API-qualified key and the same parameter pointer. A nested
evaluate from another API restores the previous binding on unwind. Without a creation in
that API, missing dimensions remain unknown even if another API has the same handle.

Do not modify retoque/dlssnr/DlssNrFeature_Vk.cpp in this follow-up: another agent owns it.
The input scope reaches native Vulkan's existing GetRenderSize call with the same parameter
block as well. Any native caller bypassing these NGX input hooks still requires explicit
metadata or a future binding at its own lifetime boundary; it cannot borrow a global feature.


## Bounds and pre-upscale motion

The valid render rectangle and each allocation remain separate. Clamp bases and extents
independently for depth and motion. Never enlarge a known valid area to fill allocation.
A base at/past the end produces an empty area and is refused before a model evaluate.
Keep the original render/display coordinate reference even when one guide's available
allocation clips its region: a clipped texture does not redefine the units of motion.

Stage=1 writes render-sized colour but may receive display-resolution vectors.
Carry display dimensions separately in DlssNrFrameInfo (CPU-only, not shader constants).
Use current OutWidth/Height, or the compatible creation display extent, as the reference
for display vectors. Low-resolution vectors use the original resolved render extent.
Use display dimensions as a complete pair; a partial pair never borrows an axis from output.
Choose the motion region from that flag/reference, then clamp it against its own resource
and base, never against the depth allocation. Thus render colour 1280x720 plus 1920x1080
motion reads the full display motion area and rescales by 1280/1920 when matched to colour.
A Stage=1 evaluate without known display dimensions and with display-resolution vectors
is refused, rather than assuming its scratch colour describes the display field.

After-upscale/host callers without an explicit display extent retain their output extent
as the display reference; present/bridge snapshots copy the CPU frame metadata normally.
No size is appended to DlssNrConstants or HLSL and neither shader target changes.

## Tests and acceptance

Before code, host controls must fail against the old resolver/dispatch geometry.
Cover creation fallback with poisoned margins, explicit metadata priority, 1:1 rendering,
missing output keys, incomplete subrects, DRS change then missing metadata, recreation,
foreign/nested parameter blocks, release/handle reuse, display changes and NR off.
Compile the production geometry slice and check per-resource bounds and scales for
MVLowRes on/off, render/display MVs, offsets, oversized extents and empty regions.
Exercise the real pre-upscale boundary with fakes so display metadata survives the scratch
swap and invalid geometry leaves the game's parameters/resources untouched.

Run affected suites, clang-format22, a Release build under the shared flock -o lease,
and all host suites with two workers. Build is compile/link evidence; host controls do
not establish NGX/GPU image quality or DRS compatibility in MSFS. No game/deploy/push.

Follow-up controls execute the real input snapshot/registration/scope/release statements and
GatherGuideFrame against fakes: missing evaluate dimensions after each API's valid creation,
no creation, identical numeric handles, isolated release/DRS/recreation and NR off. These are
CPU controls, not execution of the complete graphics input functions or either bridge on GPU.
