# Feature-scoped render geometry

`python3 tests/nr-render-geometry/run.py` compiles the production resolver/context against real NGX declarations and scripted parameters under ASan/UBSan.
The baseline adapter only omits the not-yet-existing feature registration API; all extent assertions still execute the production GetRenderSize.
Controls cover poisoned margins, identity/nesting/off, incomplete subrects, DRS, recreation, release, display epochs and 1:1/current dimensions. No NGX/device/GPU execution.
