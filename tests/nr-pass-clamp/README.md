# `nr-pass-clamp`

Host contracts and mutant checks for D3D12 interpass clamping and `PassFeedback` between sequential DLSS-NR passes
([design](../../docs/dlssnr/design/2026-11-22-interpass-clamp.md)).

## What it verifies

1. `cases.cpp` compiles `retoque/dlssnr/DlssNr_PassClamp.h` and the shared shader header
   `retoque/shaders/dlssnr/precompile/dlssnr_pass_clamp_rule.h` under ASan/UBSan:
   - Default-off gate (`PassClamp=false`, `PassFeedback=1.0`, or `Passes=1` leaves the interpass pass inactive;
     `NeedsSecondScratch` is false for 2 passes and true only for 3+ active passes).
   - Bit-identical finite passthrough when `clampEnabled == 0` and `passFeedback == 1.0`.
   - NaN/Inf sanitization falls back to that pass's own input (`sanProxy`), never mid-grey `0.5`, and non-finite
     proxy channels fall back to `0.0`.
   - Hue-preserving `CubeScaleResidual` scaling on `[0, 1]^3` proxies and representability preservation for
     passthrough float proxies outside `[0, 1]`.
   - `PassFeedback` linear under-relaxation (`0.0`, `0.5`, `1.0`, NaN fallback).
   - Zero SRV/UAV texture aliasing and same-temporal-age proxy selection across 2..10 sequential passes.
   - Downstream `passReset` arming.
2. Eight rule mutants compile against `cases.cpp` and must fail at runtime.
3. Source guards over `DlssNr_Dx12*.cpp` and `OptiScaler.vcxproj(.filters)` verify registration, boundary routing,
   `prevPassInput` advancement, `MakeRoomForGeneration`, `ParkAltSurfaces` and `ReleaseSurfacesIfFormatChanged`,
   with four source mutants rejected.

## What it does not verify

No GPU, D3D12 runtime, NGX feature evaluation, native Windows driver behavior, or in-game visual quality is exercised.
