// Per-pixel interpass clamp and feedback rule shared by dlssnr_pass_clamp.hlsl and tests/nr-pass-clamp.
// See docs/dlssnr/design/2026-11-22-interpass-clamp.md.
//
// Hue-preserving CubeScaleResidual interpass clamp, NaN/Inf sanitization, and PassFeedback under-relaxation
// adapted from janblade's OptiScaler-F5-DLSSNR-Multipass PRs #7, #18, and #41 (GPL-3.0,
// https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass), building on hhkbble's CubeScaleResidual.
//
// Plain C/HLSL subset: define PC_FN (`inline` in C++, empty in HLSL) and PC_UNROLL (`[unroll]` in HLSL,
// empty in C++) before including this header.

#ifndef DLSSNR_PASS_CLAMP_RULE_H
#define DLSSNR_PASS_CLAMP_RULE_H

PC_FN float PassClampFinite(float v, float fallback) { return isfinite(v) ? v : fallback; }

PC_FN float3 PassClampFinite3(float3 v, float3 fallback)
{
    return float3(PassClampFinite(v.x, fallback.x), PassClampFinite(v.y, fallback.y), PassClampFinite(v.z, fallback.z));
}

// Scale (target - proxy) by one scalar in [0, 1] so no channel leaves [min(0, proxy), max(1, proxy)].
// For every proxy in [0, 1]^3 (all encoded HDR frames and UNORM SDR frames) the bounds are [0, 1]^3,
// identical to CubeScaleResidual. When a passthrough float proxy sits outside [0, 1], expanding the
// box to include proxy keeps proxy representable (no per-channel crush) and keeps (hi - proxy) >= 0.
PC_FN float3 PassClampCubeScale(float3 proxy, float3 target)
{
    const float3 lo = min(proxy, float3(0.0f, 0.0f, 0.0f));
    const float3 hi = max(proxy, float3(1.0f, 1.0f, 1.0f));
    const float3 d = target - proxy;
    float alpha = 1.0f;

    if (d.x > 1e-6f)
        alpha = min(alpha, (hi.x - proxy.x) / d.x);
    else if (d.x < -1e-6f)
        alpha = min(alpha, (lo.x - proxy.x) / d.x);

    if (d.y > 1e-6f)
        alpha = min(alpha, (hi.y - proxy.y) / d.y);
    else if (d.y < -1e-6f)
        alpha = min(alpha, (lo.y - proxy.y) / d.y);

    if (d.z > 1e-6f)
        alpha = min(alpha, (hi.z - proxy.z) / d.z);
    else if (d.z < -1e-6f)
        alpha = min(alpha, (lo.z - proxy.z) / d.z);

    if (alpha >= 1.0f)
        return clamp(target, lo, hi);

    return clamp(proxy + saturate(alpha) * d, lo, hi);
}

// One interpass hand-off step: sanitize NaN/Inf (falling back to that pass's own input), optionally
// restore range hue-preservingly against that input, and optionally under-relax toward that input.
// The >= 1.0 branch avoids lerp rounding so PassFeedback = 1.0 is bit-identical to `restored`.
PC_FN float3 PassClampStep(float3 raw, float3 proxy, unsigned int clampEnabled, float passFeedback)
{
    const float3 sanProxy = PassClampFinite3(proxy, float3(0.0f, 0.0f, 0.0f));
    const float3 sanRaw = PassClampFinite3(raw, sanProxy);
    const float3 restored = clampEnabled != 0u ? PassClampCubeScale(sanProxy, sanRaw) : sanRaw;
    const float feedback = saturate(PassClampFinite(passFeedback, 1.0f));
    return feedback >= 1.0f ? restored : sanProxy + feedback * (restored - sanProxy);
}

#endif // DLSSNR_PASS_CLAMP_RULE_H
