# The Vulkan guide resample on a real Vulkan device

Run `python3 tests/guide-match-vk/run.py` outside a build window. It refuses to start while `MSBuild`,
`build-local.sh` or `dxc.exe` is running; with other agents on the machine, run it under
`flock -o ~/.cache/retoque-wt/msvc-prefix.lock`. `--build-only` and `--run-only` split compilation from execution, and
`--run-only` refuses sources changed since the build.

Output goes to the ignored `artifacts/`:

- `run.log` (one line per case, and the first mismatch if any);
- `run/guide-match-report.json` (every case's counts);
- `result.json` (PASS, FAIL or SKIP, with the reasons).

Design: `docs/dlssnr/design/reduced-scale-guides-vk.md`.

## What it runs

The production `DlssNr_GuideMatch_Vk` (`retoque/shaders/dlssnr/DlssNr_GuideMatch_Vk.*`, with its SPIR-V from
`precompile/dlssnr_guides.hlsl` under `VK_MODE`), compiled by MSVC with a `pch.h` shim in a private copy of the
compiler prefix. It runs in an isolated Wine prefix on winevulkan, which forwards to the machine's driver.

- The instance asks for Vulkan 1.1, the floor of DLSS's Vulkan path. The device enables no extension and no feature,
  because the pass needs none. No NGX, no OptiScaler.dll, no game.
- It is called the way the Vulkan after-upscale route calls it (`DlssNrFeature_Vk.cpp`):
  - the pass is made once on the device, and its refusal rule is asked about the views first;
  - the game's depth and motion are uploaded into images of their own formats and left in `SHADER_READ_ONLY_OPTIMAL`,
    as NGX takes them;
  - the outputs are made with the route's formats and usage and never initialised;
  - `Record` goes into a command buffer, and the outputs are copied out after the pass's own closing barrier.
- **The content is analytic.** Every source texel holds a value of its own, so each output texel names the texel it
  was read from, and it must equal `DlssNr::GuideMatch::PointSource`'s prediction bit for bit:
  - depth is a distinct value in [0, 1);
  - motion is the column and row, exact in the format;
  - both shift with the frame.

| Case | Depth | Motion | Out | Frames |
|---|---|---|---|---|
| `doom_quality_half` | `D32_SFLOAT`, 2293x960 at (5,3) of 2304x968 | `R32G32_SFLOAT` 2293x960 | 1720x720 | 1 |
| `display_motion` | `R32_SFLOAT` 2293x960 | `R32G32_SFLOAT` 3440x1440 (display-resolution vectors) | 1720x720 | 1 |
| `route_harness` | `R32_SFLOAT` 960x540 | `R16G16_SFLOAT` 960x540 | 640x360 | 1 |
| `half_d32s8` | a depth view of `D32_SFLOAT_S8_UINT` 1280x720 (exact 2:1) | `R16G16_SFLOAT` 960x720 at (7,9) | 640x360 | 1 |
| `unaligned` | `R32_SFLOAT` 1031x775 at (1,2) | `R32G32_SFLOAT` the same | 517x389 | 1 |
| `doom_inflight` | as `doom_quality_half` | as `doom_quality_half` | 1720x720 | 20, three in flight |

**`doom_inflight`** changes the inputs every frame and shares the outputs, as the route does. Each frame's read-back is
judged when its slot comes round again. So the pass's own barriers are what order one frame's write after the last
one's read, and its ring of 16 descriptor sets wraps.

**Validation** is the Khronos layer's core and synchronization checks.

- Each is proved active by a negative control: an invalid fence flag, and two unordered clears.
- Zero errors are required, counted after `vkDestroyDevice`.
- winevulkan's own extension additions to the host-side device are set aside and counted
  (`wine_injected_extension_messages`); the application enables none.

## Measured

- **2026-10-23** (RTX 4090, driver 615.78.08, Khronos validation layer 1.4.363, under Wine): PASS. Every texel of all six cases was exact
  (about 27.9 million in all), with 0 validation errors.
- **Two mutations, each reverted, show it can fail:**
  - Without the pass's barrier before its write: 10 validation errors, `VUID-vkCmdDraw-None-09600`, the outputs used in
    a layout other than the one bound.
  - With the depth region's base shifted by one texel in `MakePush`: every depth texel of every case differs.

## SKIP

Exit 0 with `"status": "SKIP"` in `result.json` when `DlssNr_GuideMatch_Vk::Unsupported` refuses the device: no
`R32_SFLOAT` or `R32G32_SFLOAT` storage and sampling, or no image of the route's usage at 1720x720.

`half_d32s8` alone may be skipped, when the device cannot sample `D32_SFLOAT_S8_UINT`.

## Limits

- It does not exercise the game-side unknowns: what a real game's guides contain, their layout if a game strays from
  NGX's convention, or the model's reading of the outputs.
- One device and one queue.
- That the model reads these outputs is `tests/vulkan-present-host`'s `nr-route-guides`.
