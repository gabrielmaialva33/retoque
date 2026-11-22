# `nr-dejitter`

Host contracts and mutant checks for D3D12 post-upscale motion-vector de-jittering (`[DlssNr] DejitterMotionVectors`,
[design](../../docs/dlssnr/design/2026-11-22-dejitter-motion-vectors.md)).

## What it verifies

1. `cases.cpp` compiles `retoque/dlssnr/DlssNr_Dejitter.h` and the shared shader header
   `retoque/shaders/dlssnr/precompile/dlssnr_dejitter_rule.h` under ASan/UBSan:
   - Default-off and eligibility guards (`enabled=false`, `beforeUpscale=true`, `held=true`, missing `gameGuides`,
     missing `MVJittered` or `MVLowRes`, display-resolution motion vectors, missing or non-finite jitter, tiny/non-finite
     `MV_Scale`, or missing render sequence).
   - First-frame priming (`pulseReset=true`, `dispatch=false`), consecutive-frame `(jitterNow - jitterPrev) / mvScale`
     sign cancellation on static geometry, zero-delta dispatch skip, and single reset pulse on disable.
   - History re-priming and `pulseReset` across sequence gaps, explicit resets, render/output size changes, route or
     epoch changes, and held-frame release.
   - Non-finite and float-overflow passthrough in `ApplyMotionDejitter`.
2. Eight rule mutants compile against `cases.cpp` and must fail at runtime.
3. Source guards over `DlssNr_Dx12*.cpp` and `OptiScaler.vcxproj(.filters)` verify NGX jitter/flag reads, pre-`GuideMatch`
   dispatch ordering, `rawMotionIn` preservation on final `Resolve`, Present render-sequence forwarding, and resource/shader
   retirement in `ParkUntrackedGeneration` and `MakeRoomForGeneration`, with six source mutants rejected.

## What it does not verify

No GPU, D3D12 runtime, NGX feature evaluation, native Windows driver behavior, or in-game visual quality is exercised.
