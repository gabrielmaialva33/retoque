# De-jittered motion vectors for post-upscale DLSS-NR (2026-11-22)

Status: implemented on D3D12 behind `[DlssNr] DejitterMotionVectors` (default `false`), with a D3D12-only menu control
(`De-jitter motion vectors`) in the full NR panel; host-tested in `tests/nr-dejitter`, `tests/nr-invariants`,
`tests/nr-menu` and `tests/nr-localization`; **not yet built with MSVC or run on a GPU or in a game**. Native Vulkan is
not covered and does not offer the control.

## Problem

After DLSS Super Resolution (or another temporal upscaler) reconstructs an unjittered display-resolution colour buffer,
many engines still pass render-resolution motion vectors flagged with `NVSDK_NGX_DLSS_Feature_Flags_MVLowRes` and
`NVSDK_NGX_DLSS_Feature_Flags_MVJittered`. Those vectors include the sub-pixel camera jitter step
$-(\text{jitter}_t - \text{jitter}_{t-1}) / \text{mvScale}$ between consecutive render frames. Handing jitter-contaminated
vectors alongside an already reconstructed (unjittered) post-upscale colour frame causes DLSS-NR's recurrent history
and any motion-guided consumers (`GuideMatch`, peripheral warp, model cadence carry) to Advect static detail by the
Halton jitter step every frame, producing fine-detail shimmer on still cameras.

## Source and licence

Adapted from **kkyleeb21's** [`OptiScaler_DLSSNR`](https://github.com/kkyleeb21/OptiScaler_DLSSNR) D18 research branch
([`nr_jitter_policy.h`](https://github.com/kkyleeb21/OptiScaler_DLSSNR/blob/research/dlssnr-internal-scaling-d18/tools/D24/bg3-native/nr_jitter_policy.h)
and
[`NativeControl.h`](https://github.com/kkyleeb21/OptiScaler_DLSSNR/blob/research/dlssnr-internal-scaling-d18/OptiScaler/dlssnr/NativeControl.h),
GPL-3.0, inherited from OptiScaler).

## How it works

1. **Opt-in default identity.** `[DlssNr] DejitterMotionVectors` defaults to `false` (`auto`). When disabled, no
   `g_nr.motionDejittered` scratch texture or `DlssNr_Dejitter_Dx12` pipeline is created, no extra barriers run, and
   all default/disabled paths stay byte-for-byte unchanged.
2. **Input collection (`DlssNr_Dx12_Inputs.cpp`, `DlssNr_Dx12.cpp`).**
   - `GatherGuideFrame` records `MotionVectorsJittered` (`NVSDK_NGX_DLSS_Feature_Flags_MVJittered`) and finite
     `JitterOffsetX` / `JitterOffsetY` (`NVSDK_NGX_Parameter_Jitter_Offset_X/Y`, gated by `HaveJitter`) alongside the
     existing `MotionVectorsLowResolution` and `MvScaleX/Y` fields.
   - Consecutive render-frame identity comes from `g_upscaleRenders` on direct post-upscale evaluations and from the
     present-guide ring slot's `sequence` (`frame.RenderSequence` / `g_bridgeRead.slot`) on Present/bridge evaluations,
     so repeated presents or skipped guide captures cannot look like consecutive render frames.
3. **Eligibility and continuity policy (`retoque/dlssnr/DlssNr_Dejitter.h`).**
   - `DlssNr::Dejitter::Eligible` requires: `enabled`, `!beforeUpscale` (pre-upscale colour is itself jittered at
     render resolution), `!held`, `gameGuides`, `mvJittered`, `mvLowResolution`, `haveJitter`, finite jitter offsets,
     finite `mvScaleX/Y` with `|scale| > 1e-6`, non-zero render and output dimensions, `motionWidth == renderWidth` and
     `motionHeight == renderHeight`, and `renderSequence != 0`.
   - On the first eligible frame, or whenever `reset` is true, `renderSequence != prevRenderSequence + 1`, or
     render/output size, `mvScale`, route (`AfterUpscale` / `Present` / `NativeDx11Scene`), or `pictureEpoch` changes,
     `Step` primes `prevJitter` and pulses `pulseReset = true` without dispatching a stale delta. Turning the setting
     off after an active run also pulses a single NR reset (`g_nr.reset`, `g_nr.passReset`, and `Cadence::Signature`
     mix) so recurrent state drops cleanly.
   - On consecutive eligible frames, `deltaMv = (jitterNow - jitterPrev) / mvScale`. If `deltaMv == (0, 0)`, the GPU
     dispatch is skipped without resetting history.
4. **D3D12 compute pass (`DlssNr_Dejitter_Dx12`, `dlssnr_dejitter.hlsl`, `dlssnr_dejitter_rule.h`).**
   - Runs after `UpdateMotionProbe` (so zero-motion detection still inspects the game's raw vectors) and before
     `GuideMatch`, peripheral guide packing, model cadence carry, enlargement, and `evaluate`.
   - Writes `ApplyMotionDejitter(mv, deltaMv)` (adding `deltaMv` to finite vectors and leaving any non-finite or
     overflowing vector untouched) into `g_nr.motionDejittered` (`DXGI_FORMAT_R32G32_FLOAT`, extent
     `(motionBaseX + motionWidth) x (motionBaseY + motionHeight)`), transitions it to `NON_PIXEL_SHADER_RESOURCE` for
     downstream guide consumers, and restores `UNORDERED_ACCESS` via `RestoreResourceState` at scope exit.
   - Preserves `rawMotionIn` on the final `Resolve` `DispatchPass`.
   - Retires `g_nr.motionDejittered` and `g_nr.dejitterPass` through `ParkNrResource`, `ParkUntrackedGeneration`,
     `MakeRoomForGeneration`, `TickNrRetired`, and `Shutdown`.

## How to measure in a game

1. Choose a D3D12 title using DLSS Super Resolution that flags `MVLowRes | MVJittered` and supplies camera jitter
   offsets (for example an Unreal Engine or BG3-style D3D12 scene with fine foliage, grates, or specular edges on a
   stationary camera).
2. With `Stage=0` (after upscaler) and `DejitterMotionVectors=false`, record a 60-frame stationary baseline using
   **Measure detail** (`[DlssNr] DetailMeasure`) and the **Temporal pulse** (`[DlssNr] PulseMetric`) readout.
3. Enable `DejitterMotionVectors=true` on the identical stationary view and repeat both measurements: verify lower
   frame-to-frame temporal step / shimmer in `PulseMetric` and `DetailMeasure` temporal variance without ghosting when
   panning the camera or cutting between views.

## What is not verified

- Full MSVC DLL build (`./build-local.sh` is run by the lead, not in this worker).
- GPU execution under Proton/vkd3d-proton and native Windows D3D12 (required for any new D3D12 compute pass).
- Native Vulkan (`DlssNrFeature_Vk.cpp` is untouched and hides the menu control).
- Per-game convention audit on titles that clear or misreport `NVSDK_NGX_DLSS_Feature_Flags_MVJittered`.
