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
