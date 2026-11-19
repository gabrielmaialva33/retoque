# In-game Neural Rendering panel, without a game

This tool draws **the complete production `DlssNr_Menu.cpp` panel**, using the repository's Dear ImGui,
into PNGs with a small CPU triangle rasterizer. It opens no window, uses no GPU or Wine, and never loads a
model, inspects a game, writes settings, or installs anything. The idea of a production-page fixture came
from NeuRotic's README; no source, data, translations or assets were copied from that fork for this tool.

## Run

Requires Python 3.11+, a C++20 compiler (`g++` by default; `CXX` can select another), `pkg-config`, and
FreeType development headers/library (`pkg-config freetype2`). These are the only host dependencies;
nothing is downloaded or installed by the script. The DLL's ImGui configuration already selects FreeType,
so the preview uses that same font backend instead of silently substituting stb_truetype.

From any directory:

```bash
python3 tools/menu-preview/run.py --output /path/outside/the/repository/menu-pictures
python3 tools/menu-preview/run.py --output /path/outside/the/repository/menu-pictures --state d3d12-simple
python3 tools/menu-preview/run.py --output /path/outside/the/repository/menu-pictures --state present-hdr-pt-BR
```

Without `--output`, a persistent `retoque-menu-pictures-*` directory is created under Python's system
temporary directory (`TMPDIR` is honored). Build files use a separate temporary directory and are deleted
on completion. Output inside this repository is refused. Existing files for selected states are replaced;
other files are left alone. Each state runs in a fresh process, as the real menu's static translated labels
and pending values are process scoped. Output is a PNG, an ImGui text transcript and a small JSON file with
size, triangle count, font-texture coverage and content height. The latter two are evidence for this tool,
not instrumentation of a real game. No timestamps or host paths enter the PNG.

| State | Fixture |
| --- | --- |
| `nr-off` | NR disabled, simple view |
| `d3d12-simple` | D3D12 after upscaling, NR enabled, simple view |
| `d3d12-full` | Same, Show all settings enabled |
| `individual-passes` | Three passes, individual tuning, explicit preset/intensity for pass 1, structure for pass 2 |
| `present-hdr` | Present route, HDR10 conversion and synthetic 203-nit display white |
| `native-vulkan` | Native Vulkan, full view, no legacy timing value |
| `model-missing` | Enabled but model unavailable, real missing-model reason and Retry |
| `comparison` | Full view, held frame, wipe, labels and split controls |
| `d3d12-simple-pt-BR` | Simple view with the shipped optional Portuguese dictionary |
| `present-hdr-pt-BR` | HDR present with the shipped optional Portuguese dictionary |

The fixed canvas is 560 x 560 for simple states and 560 x 2600 for full states. This is an isolated,
long-form panel view at scale 1, with a synthetic title/watermark and enough height to inspect all sections.
It does **not** reproduce the main window's sidebar, footer, card border, viewport clipping or actual available
width (which varies with translated tab labels). Long unwrapped text can be clipped horizontally exactly as
ImGui draws it at the fixture width. Vertical clipping is rejected by the suite. Blank space at the bottom
is intentional: states have fixed sizes for comparison. Do not interpret this as the real window's height.

## What is real and what is fake

`generate.py` slices the complete `namespace DlssNr` body verbatim from
[DlssNr_Menu.cpp](../../retoque/dlssnr/DlssNr_Menu.cpp). It also extracts the actual
`MenuCommon::ApplyThemeStyle`, `ScopedIndent`, `ScopedCollapsingHeader`, the needed Config declarations/defaults
and runtime status structs. It does not rewrite controls, remove functions or change production files.
Changed source anchors fail the generation/compilation instead of quietly falling back to a copied panel.

The build compiles the four repository ImGui translation units and its FreeType backend. It uses the embedded
Hack font at the menu's default 14 px, the real dark theme, and scale 1. The `MenuFonts` translation-fallback
planner does nothing for English/pt-BR; its Windows system-font discovery is therefore not linked.
`Localization::Dictionary` is the real parser/lookup/label-identity implementation; only the runtime dictionary
loading location is replaced with the explicit repository pack path.

`facade.h` supplies synthetic Config/State and runtime getters. Getters report fixture values, empty observations
or unavailable measurements, and action methods have no external effects. It is not the runtime Config lock,
transaction or renderer implementation. Every visible value is synthetic, including pass counts, Vulkan frame
count, model availability, HDR status and capability flags. The HDR menu preview is an ordinary SDR PNG;
HDR display encoding/tone mapping (`MenuHdrCheck`) is deliberately a no-op.

`raster.h` is original code: pixel-centre sampling, top-left triangle edge ownership, bilinear RGBA font-atlas
sampling, vertex-colour interpolation, clipping and source-over blending. It handles ImGui 1.92's dynamic atlas
updates and vertex/index offsets. Unsupported callbacks or non-atlas textures fail loudly. PNG encoding reuses
[DlssNr_ImageFile.h](../../retoque/dlssnr/DlssNr_ImageFile.h); no image library was added.
See [attribution](../../Licenses/MenuPreview_ATTRIBUTION.txt).

## Validation and limits

```bash
python3 tests/run_all.py --only menu-preview,nr-menu,repo-docs,repo-privacy --jobs 1 --strict
```

The [suite](../../tests/menu-preview/README.md) verifies the rasterizer's edge/alpha/clip rules, decodes and
checks every PNG, requires font texture coverage and bright panel text, rejects vertical clipping, and compares
two fresh-process renders byte for byte. Exact bytes are promised for repeat runs of the **same build and
FreeType version**, not across compiler, font-engine or upstream ImGui upgrades. There are no checked-in goldens.

A preview is **not a game run**: no GPU state, real values, timing, image quality, performance, driver behavior,
HDR output correctness or native Windows validation is implied. These fixtures do not test clicking, hovering,
scrolling, capture/clipboard actions, or every conditional panel state; use the existing host contracts and
runtime validation separately. The whole panel compiles unchanged, so no production seam is needed for it.
