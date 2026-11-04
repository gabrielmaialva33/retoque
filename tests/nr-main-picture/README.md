# Main picture

`python3 tests/nr-main-picture/run.py`

A game can keep more than one DLSS feature alive and evaluate them in turn (a 3D character preview next to the world).
NR follows one feature, the main picture, and leaves any other one as it is
([main-picture.md](../../docs/dlssnr/design/main-picture.md)).

The suite compiles `retoque/dlssnr/DlssNr_Pictures.h` and its glue `DlssNr_PictureGate.h`, with the production
render-geometry binding, under ASan/UBSan against fake parameter blocks:

- one feature: always main, made again after a release or before it (quiet handover), no history restart on First or
  Vacant;
- the world with a preview in either order, at different sizes and at one size: the preview is left aside, said once;
- a preview made first: the clearly larger world takes main and restarts the history; a quarter more area is the line,
  an unknown area never takes;
- views of main on one colour allocation (one feature per screen): views, never aside or a takeover, and they keep
  main alive; an unknown colour is never a view;
- a quiet main (never released) handed over after `kQuietCalls` calls by others, never while main alternates;
- main released while the preview is open: a new feature takes main at once, the known preview waits `kQuietCalls`;
- the glue: no binding, a foreign parameter block or a disabled scope is main without asking the rule; one judgement
  per evaluate shared by both seams; `RenderGeometry::Forget` tells the rule.

Picture epochs: the epoch gate in both arrival orders, unjudged frames and a holder's first epoch; a known other
picture taking main after the wait is a new picture. Thirty rule and glue mutants must fail those cases. Source guards read the four NR seams
(`ScopedPreUpscale`, `EvaluateAfterUpscale`, `EvaluateAfterUpscaleVk`, `CarryNativeDx11Guides`): each asks right after
its NR-off return and before any other way out, its coverage, lock or guide carry, reading the colour only when there
is an evaluate to judge; an aside picture returns without declining, reporting or touching the status, coverage or
history flags. `GatherGuideFrame` puts the picture epoch in the frame, and `Dispatch` (under `g_nrMutex`) and the
Vulkan route (under `g_vkMutex`) consume it: a newer picture restarts the history and invalidates the exposure meter,
an older one is refused. Twenty-one source mutants must be rejected.

Not proved here: the feature layout of any real game (Neverness to Everness has not been run with this build), the
picture NR follows after a takeover on a GPU, or anything on native Windows.
