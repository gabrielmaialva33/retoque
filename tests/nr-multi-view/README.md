# Multi-view stand-down

`python3 tests/nr-multi-view/run.py`

A game that draws several views into one colour allocation within one frame (a triple-screen rig, split screen)
calls the upscaler once per view. NR keeps one model and one history, so since 2026-10-30 it stands down on every
evaluate of such a frame, counted and logged ([dx12-robustness.md](../../docs/dlssnr/design/dx12-robustness.md)).
The detector is janblade's `1105908f` (`DlssNr_Views.h`), detection and default stand-down only.

The suite compiles `retoque/dlssnr/DlssNr_MultiView.h` under ASan/UBSan against synthetic sub-rectangles: one view
per frame with dynamic resolution (never a layout), three screens plus the combined picture (found on the second
frame, every evaluate after it stood down and counted), two equal views at different offsets, pictures in different
allocations (not this layout), a present counter that never advances, no observed present yet, an isolated
multi-view frame, the 300-frame release and its restart, and more views than are remembered. Seven rule mutants,
the first being the behaviour before the detector (every evaluate runs), must fail those cases.

Source guards read both D3D12 seams: `ScopedPreUpscale` asks after it owns the frame and before any scratch loan or
colour read, and stands down rather than declining; `EvaluateAfterUpscale` asks after the present-route deferral and
its render count, before the pass is built or dispatched; neither has a view pick or setting. `nr-before-upscale`
runs the same layout through the production seams in both stages.

Not proved here: the layout of any real game (Assetto Corsa EVO's rig has not been run with this build), the cost of
the parameter reads, or anything on a GPU.
