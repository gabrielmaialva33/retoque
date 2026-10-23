# Feature-scoped render geometry

`python3 tests/nr-render-geometry/run.py` compiles the production resolver/context against real NGX declarations and scripted parameters under ASan/UBSan.
The baseline adapter only omits the not-yet-existing feature registration API; all extent assertions still execute the production GetRenderSize.
Controls cover poisoned margins, identity/nesting/off, incomplete subrects, DRS, recreation, release, display epochs and 1:1/current dimensions. No NGX/device/GPU execution.

The runner also compiles the verbatim dispatch geometry/reference blocks and `GatherGuideFrame`
with the real CPU frame type. Controls cover Stage 1 render scratch with display MVs, MVLowRes
on/off, independent offsets and allocation clipping, empty regions, poisoned motion margins,
original units for motion scale, and creation/current display metadata. The boundary suite
`nr-before-upscale` separately checks that missing display metadata declines before allocation
for display MVs while low-resolution MVs remain usable. Host fakes do not prove GPU sampling.

Incomplete/invalid Width/Height must retire the fallback for the next evaluate too, and the DRS
control starts from a fresh epoch rather than inheriting an earlier partial-subrect invalidation.
Display reference axes are accepted as a pair, including on host/output fallback paths.

## D3D11/Vulkan bridge follow-up

The runner extracts the production geometry snapshot, Remember, Scope and Forget statements
from all three NGX input lifetimes, preserving snapshot order relative to the backend factory.
It executes them around real GatherGuideFrame and the actual Dispatch unknown-geometry guard.
A fake backend normalizes creation dimensions to 1x1, so a snapshot taken too late fails.
No graphics-input function or bridge is executed as a whole by this host control.

Separate D3D11 and Vulkan cases require creation fallback without evaluate dimensions and
refusal without creation. A shared numeric handle then proves API isolation for dimensions,
release and DRS, recreation, disabled NR and full 32-bit IDs. The old source-only integration
guards were replaced by these executed lifetime controls. No new suite/registry entry is needed.
