# Render geometry: creation fallback and pre-upscale display motion

2026-10-23. Scope: D3D12 NGX evaluates (including callers routed through that input),
CPU geometry contracts only. No new setting, shader math or cbuffer change.

## Problem and decisions

A missing evaluate subrect/Width/Height currently becomes the guide allocation extent.
MSFS-shaped allocations can have invalid margins. ShyVortex 491e5af9/v0.9.71/72 falls
back through parameter dimensions to the current feature. We retain that priority but
never infer identity/freshness from the global currentFeature pointer: pre-upscale is
entered before the selected backend updates that pointer, and RenderWidth is mutable.

Record the successful upscaler creation's render/display dimensions by NGX handle,
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
axes. Once an explicit render size differs from creation (DRS), missing dimensions in
that epoch cannot fall back to creation again, even after the current size returns to
the original. A new creation re-arms the fallback. A display-size change also makes the
old creation fallback stale. Complete current metadata remains usable after either event.
No inference can detect an engine silently changing its valid area without metadata;
creation fallback relies on the feature contract until contradictory metadata arrives.
This is an explicit limit, not a freshness guarantee based on allocation size or time.

Frame gathering carries whether render geometry is known and its source. Unknown or
incomplete real-game guides are refused before recording NR. Zero/synthesized guides
remain governed by their existing non-game-guide contract. Native Vulkan creation and
D3D11-native creation are not registered by this D3D12 input change; they keep explicit
metadata resolution, and do not borrow another API's creation fallback.

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
