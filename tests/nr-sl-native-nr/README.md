# The game's own Streamline NR

`python3 tests/nr-sl-native-nr/run.py`

A game that runs DLSS Neural Rendering through Streamline 2.14's `sl.dlss_nr` (feature 1004) while Retoque's NR is on
would have the picture given NR twice. Since 2026-10-30 every NR route stands down while the game's runs, counted and
logged, with a line in the panel ([streamline-native-nr.md](../../docs/dlssnr/design/streamline-native-nr.md)).

The suite compiles `retoque/dlssnr/DlssNr_SlNativeNr.h` under ASan/UBSan against synthetic Streamline calls: the
constants as Streamline v2.14.1 declares them; a load request in slInit (said once, never a stand-down); every other
feature's evaluate (ignored); a refused NR evaluate (counted, said, never a stand-down); an accepted one (latched,
said, every later ask counted); the 5 s quiet window at its boundary, the release and a new latch; a game evaluating
every frame; a hook clock ahead of the asking thread; an evaluate older than the window when NR is switched on; the
uplift colour and control-mask tags (recorded, said once, deciding nothing; a tag without a resource is none);
`sl::ResourceTag`'s shape through `NoteTags`; everything on one ask; the bounded count line; the process instance and
the hooks' entry points; and notes and asks from several threads. Thirteen rule mutants, the first being the
behaviour before the detector (NR runs on top of the game's), must fail those cases; only the two that change a
constant may be caught by the compiler.

Source guards read the hooks (slInit noted before OptiScaler edits the preferences; the evaluate noted after
Streamline answered, as `eOk`; the tag hooks noting before any return; no hook decides; slInit and slEvaluateFeature
attached outside the frame-generation gate), the D3D12 routes (`EvaluateAfterUpscale`, `ScopedPreUpscale` standing
down rather than declining, `RunPresentPass` before any copy, and `Dispatch` as the backstop beside
`PausedWithoutFrameGen`, before any recording), the Vulkan after-upscale route (before the model is claimed), the
Vulkan present host (`NrSkip::GameNr` before the claim, no log under its lock, nothing freed), the panel line (only
with NR on, never a control, in the pt-BR pack), the vendored 2.11.1 headers (not edited) and the design record.
Sixteen source mutants must each fail a guard.

Not proved here: that any game drives its NR through `slEvaluateFeature(1004)` (the public SDK has no `sl_dlss_nr.h`),
that an accepted evaluate means NR rendered, the picture in NBA 2K27, Portal RTX or Hogwarts Legacy, or anything on a
GPU or under Wine.
