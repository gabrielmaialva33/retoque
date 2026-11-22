# Interpass clamp and `PassFeedback` for sequential DLSS-NR passes (2026-11-22)

Status: implemented on D3D12 behind `[DlssNr] PassClamp` (default `false`) and `[DlssNr] PassFeedback` (default `1.0`),
with menu controls in `Sequential model passes` when `Passes > 1`; host-tested in `tests/nr-pass-clamp`,
`tests/nr-invariants`, `tests/nr-native-rules`, `tests/nr-menu` and `tests/nr-localization`; **not yet built with
MSVC or run on a GPU or in a game**. Native Vulkan remains single-pass (`nr-vk-passes.md`) and offers neither control.

## Problem

When `Passes > 1` on D3D12, pass $N+1$ reads pass $N$'s raw output directly (`DlssNr_Dx12.cpp`). Any out-of-range
excursion ($< 0$ or $> 1$ on an encoded HDR or UNORM proxy), hue-skewed highlight overshoot, or non-finite pixel
(`NaN` / `Inf`) emitted by pass $N$ is fed straight into pass $N+1$ and compounded into pass $N+1$'s own recurrent
temporal state before the final `Resolve` pass ever applies `MaxRatio` or `ResidualLimit`. Across 2–10 passes this
shows up as temporal pumping, highlight blotches, or dark/bright single-frame pops.

## Source and licence

Adapted from **janblade's**
[`OptiScaler-F5-DLSSNR-Multipass`](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass) pull requests
[#7](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/pull/7),
[#18](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/pull/18),
[#41](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/pull/41),
[#55](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/pull/55), and
[#61](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/pull/61) (GPL-3.0, inherited from OptiScaler), which
build on **hhkbble's** hue-preserving `CubeScaleResidual` already used in `dlssnr.hlsl`.

## How it works

1. **Opt-in default identity.** `DlssNr::PassClamp::Active(count, passClamp, passFeedback)` is `true` only when
   `Passes > 1` and either `PassClamp=true` or `PassFeedback < 1.0`. With defaults (`PassClamp=false`,
   `PassFeedback=1.0`), no scratch texture is allocated, no extra barrier is recorded, and the pass loop is
   byte-for-byte unchanged.
2. **Per-pixel rule (`retoque/shaders/dlssnr/precompile/dlssnr_pass_clamp_rule.h`).**
   - Sanitize `sanProxy = PassClampFinite3(proxy, 0)` and `sanRaw = PassClampFinite3(raw, sanProxy)`. Falling back a
     non-finite channel in `raw` to `sanProxy` (rather than a constant `0.5`) avoids mid-grey flashes on dark scenes
     while leaving every finite channel bit-identical.
   - When `PassClamp=true`, `PassClampCubeScale(sanProxy, sanRaw)` scales `d = sanRaw - sanProxy` by one scalar
     $\alpha \in [0, 1]$ against `[min(0, sanProxy), max(1, sanProxy)]`. On every `[0, 1]^3` proxy (all encoded HDR
     frames and UNORM SDR frames) this is identical to `CubeScaleResidual` on `[0, 1]^3`, preserving chromatic
     direction instead of clipping per channel; on passthrough float proxies with values outside `[0, 1]` (`DEVELOPMENT.md`
     rule 7), expanding the box to include `sanProxy` keeps the input representable without crushing super-white values.
   - `PassFeedback` blends `sanProxy + feedback * (restored - sanProxy)` with an explicit `feedback >= 1.0 ? restored`
     branch so `1.0` introduces no floating-point rounding.
3. **Zero SRV/UAV aliasing across 2..10 passes.**
   - Model outputs still alternate between `(g_nr.output, g_nr.passPing)` via `DlssNr::Chain::Routing`.
   - Interpass clamp outputs alternate between two working-size scratch UAVs: `g_nr.passClampScratch` (allocated for
     `Passes >= 2`) and `g_nr.passClampScratch2` (allocated only for `Passes >= 3`).
   - At boundary $i \in \{1, \dots, \text{Count}-1\}$, `boundary.raw` is in `{output, passPing}`, `boundary.target` is
     `(i - 1) % 2 == 0 ? passClampScratch : passClampScratch2`, and `boundary.proxy` is `prevPassInput` (`modelInput`
     for $i = 1$, and the opposite clamp scratch `(i - 2) % 2` for $i \ge 2$). All three resources at every clamp
     dispatch, and both resources at every NGX `evaluate`, are distinct textures. If pass $i$ fails, `chain.answer`
     remains pass $i-1$'s unmodified output in `UNORDERED_ACCESS`.
   - Both scratch textures participate in `MakeRoomForGeneration`, `ParkUntrackedGeneration`, `ParkAltSurfaces`,
     `ReleaseSurfacesIfFormatChanged`, `scratchContractChanged`, `resolutionChanged` and `Shutdown`.

## Audit of the "same temporal age" rule (`janblade` #55 / #61)

In `janblade` PR #55, clamping or blending pass $i \ge 2$ back toward pass 0's raw `modelInput` mixed two different
temporal recurrence depths (temporal age 0 from the single-frame proxy vs pass $i$'s recurrent history) and introduced
ghosting/shimmer, reverted in PR #61 in favor of clamping each pass strictly against **its own input**. Checking
Retoque's chain against this rule:

- **Interpass reference:** `prevPassInput` starts at `modelInput` for pass 1 (`i = 1`, whose predecessor is pass 0, fed
  by `modelInput`) and advances to `prevPassInput = input` after each pass $i$ succeeds, so boundary $i$ always clamps
  pass $i-1$'s output against pass $i-1$'s own input.
- **Model cadence:** `DlssNr_Dx12.cpp` skips all passes together on a carried frame (`if (carried)`) and records a
  carried edit only when `chain.completed == passSnapshot.Count`, so passes in a chain never run at staggered frame
  cadences. Active `PassClamp` / `PassFeedback` values are mixed into `Cadence::Signature` so live toggles invalidate
  any stored carried edit.
- **Downstream history resets:** Live per-pass tuning edits (`g_nr.passBuilt[i] != passSnapshot.Settings[i]`), skipped
  intermediate passes, and live `PassClamp` / `PassFeedback` changes now arm `g_nr.passReset[downstream] = true` for
  all downstream passes (`i .. kMaxPasses - 1`), preventing a later pass from pairing older recurrent history with a
  newly reset or re-clamped upstream pass.

## How to measure in a game

1. Enable DLSS-NR on a D3D12 title with `Passes=2` or `Passes=3` on a static camera scene with specular highlights or
   foliage.
2. Run the existing **Temporal pulse** (`[DlssNr] Pulse` / menu readout) and **Measure detail** (`DetailMeasure`, 60
   still frames) baselines with `PassClamp=false, PassFeedback=1.0`.
3. Enable `PassClamp=true` (and optionally test `PassFeedback=0.85`) on the same held/static view and compare both
   the pulse readout (step luminance variance across frames) and `Measure detail` retention to confirm reduced
   interpass pumping without excessive detail loss.

## What is not verified

- Full MSVC DLL build (`./build-local.sh` is run by the lead, not in this worker).
- GPU execution under Proton/vkd3d-proton and native Windows D3D12 (required for any new D3D12 compute pass).
- In-game visual impact and pulse/detail measurements across multi-pass presets.
