# Menu preview tool contract

Run `python3 tests/menu-preview/run.py`. Requires Python 3.11+, GCC with C++20/ASan/UBSan, `pkg-config`,
and FreeType development headers/library. A missing FreeType dependency is a failure with the package-config
error, never a silent PASS or a fallback to another rasterizer. Host execution is CPU-only and serial.

The suite builds [the preview tool](../../tools/menu-preview/README.md), which compiles the complete production
NR panel body against fixture Config/State/runtime getters and the repository's real Dear ImGui. It renders all
ten named states twice in fresh processes into temporary directories, then independently decodes PNG chunks,
checks CRCs/zlib/scanline lengths/dimensions, rejects flat pictures, requires bright text inside the panel and
font-atlas-covered pixels, and compares the two PNG byte streams. Text transcripts check that the final full-view
control was submitted; measured content height rejects vertical clipping. English/Portuguese and materially
different fixtures must produce different images. An unknown state must fail without creating output.

A small sanitized raster test independently checks a shared triangle edge (no double blend or crack), clipping,
source-over blending and bilinear font alpha. This is not an image-quality benchmark or a menu behavior test.
The existing [nr-menu suite](../nr-menu/README.md) retains its scripted-event coverage.

The pictures contain synthetic values only. Passing proves this fixture tool produces reproducible pictures,
not that the menu's decisions, live GPU values, timing, input, native Windows behavior or in-game appearance
are correct. No pictures or compiled artifacts are tracked. Pixel repeatability is local to one compiler,
ImGui/font version and FreeType build; no cross-platform golden image is claimed.
