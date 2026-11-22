// Shared HLSL/C++ motion-vector de-jitter rule (docs/dlssnr/design/2026-11-22-dejitter-motion-vectors.md).
// Adapted from kkyleeb21's OptiScaler_DLSSNR D18 research branch (GPL-3.0).

DJ_FN float2 ApplyMotionDejitter(float2 mv, float2 deltaMv)
{
    if (!isfinite(mv.x) || !isfinite(mv.y) || !isfinite(deltaMv.x) || !isfinite(deltaMv.y))
        return mv;

    float2 corrected = float2(mv.x + deltaMv.x, mv.y + deltaMv.y);
    return (isfinite(corrected.x) && isfinite(corrected.y)) ? corrected : mv;
}
