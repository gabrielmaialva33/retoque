# The Vulkan route's guides below the frame's size

Run `python3 tests/nr-vk-guides/run.py`. Design: `docs/dlssnr/design/reduced-scale-guides-vk.md`.

## What this covers

`cases.cpp` compiles the production `retoque/shaders/dlssnr/DlssNr_GuideMatch.h` and `DlssNr_GuideMatch_Vk.h` as they
are, against the real Vulkan headers in `external/vulkan`, with g++ and clang++, C++20, ASan/UBSan and `-Werror`:

- **The plan** (`GuideMatch::Choose`, `GuideMatch::ForModel`): what the model is handed, and the motion-vector scale.
  - At DOOM Eternal's numbers (3440x1440, DLSS Quality's 2293x960 guides, a 1720x720 model) the guides are matched:
    1720x720 from the origin, scale 1720 x 720.
  - When the resample is wanted and cannot run, the frame is refused. It is never handed frame-size guides.
  - `MatchGuides` off is the A/B arm: the game's regions and bases, with fix 2's scale.
  - `RenderMotionScale` off gives the legacy working / frame scale.
  - Also held: display-resolution vectors, DLSS Performance at 0.5 (nothing to resample), supersampling, one larger
    axis, and the vulkan-present-host route harness's 960x540 under 640x360.
  - For six shapes, both keys, the motion texture and scale the plan hands over equal the D3D12 route's arithmetic,
    written out in the test.
- **The game's views the resample may bind**, and why not when it may not:
  - accepted: depth-only formats, a depth view of a depth/stencil format, one-channel float colour copies;
  - refused: both aspects named, or a depth/stencil format whose aspect is not stated; stencil; integer formats;
    more than one layer; no sampled support; unknown formats;
  - motion must be a float colour format with two components or more.
- **The outputs**: `R32_SFLOAT` and `R32G32_SFLOAT`, the route's usage, and the refusal when the device lacks storage
  or sampling or the image of that usage.
- **The interface**: the push block is 48 bytes, within the 128 bytes every device guarantees, in the D3D12 cbuffer's
  order. Groups are 8x8, and the descriptor ring has 16 sets.

`run.py` then guards the sources:

- **The route** (`DlssNrFeature_Vk.cpp`, `EvaluateAfterUpscaleVk`):
  - `Choose` decides before the frame's first timestamp, barrier or dispatch, and a refused frame returns with the
    history reset set.
  - `Wanted` is asked with `MatchGuides` and the regions the route would hand over.
  - The evaluate's depth, motion, sizes, bases and scale are all the plan's.
  - The resample is recorded before the model, reading the game's views in `SHADER_READ_ONLY_OPTIMAL`.
  - Nothing of the resample's exists unless it is wanted.
  - The matched images are freed in the resize block after its drain. The pass and the images are freed only in
    `FreeRouteObjects`, and abandoned in `ShutdownVk(false)`.
- **The pass** (`DlssNr_GuideMatch_Vk.cpp`):
  - `pch.h` comes first, and the SPIR-V header is included unconditionally.
  - It has no destructor and no logging. `Destroy()` frees every object, and `Abandon()` calls nothing.
  - The push range is the block, and the four bindings have their types.
  - Before the dispatch, `UNDEFINED` to `GENERAL` from `ALL_COMMANDS`; after it, compute to `ALL_COMMANDS`.
- **The shader and the project**: the `VK_MODE` push block, bindings and storage formats; the D3D12 cbuffer still
  there; the SPIR-V and its header committed; the three project entries.

`tests/nr-invariants` holds the push block equal to the shader's `GuideParams` and to the D3D12 cbuffer, and the SPIR-V
header equal to the committed `.spv`.

## What this does not cover

No Vulkan device is created and no shader runs: these are pure functions and source guards over fakes.
`tests/guide-match-vk` runs the pass on a real device. `tests/vulkan-present-host`'s `nr-route-guides` runs it inside
the production DLL with the real model. Whether the shimmer at reduced scale goes away is for a game (DOOM Eternal at
0.5, `MatchGuides` true against false).
