#pragma once

// Portable policy and reference math for DLSS-NR interpass clamp and PassFeedback
// (docs/dlssnr/design/2026-11-22-interpass-clamp.md).
// Adapted from janblade's OptiScaler-F5-DLSSNR-Multipass PRs #7, #18, and #41 (GPL-3.0).

#include "DlssNr_PassSettings.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace DlssNr::PassClamp
{
inline float SanitizeFeedback(float configured)
{
    return std::isfinite(configured) ? std::clamp(configured, 0.0f, 1.0f) : 1.0f;
}

inline bool Active(uint32_t requestedPasses, bool clampEnabled, float configuredFeedback)
{
    return requestedPasses > 1 && (clampEnabled || SanitizeFeedback(configuredFeedback) < 1.0f);
}

inline bool NeedsSecondScratch(uint32_t requestedPasses, bool clampEnabled, float configuredFeedback)
{
    return requestedPasses > 2 && Active(requestedPasses, clampEnabled, configuredFeedback);
}

// Which scratch surface boundary `b = passIndex - 1` (for passIndex >= 1) writes into, and what
// proxy surface that boundary clamps against. Model outputs alternate (output, passPing); clamp
// outputs alternate (clamp0, clamp1), so no boundary or model pass ever aliases SRV and UAV.
template <class Surface> struct BoundaryPlan
{
    Surface raw {};
    Surface proxy {};
    Surface target {};
    uint32_t slot = 0;
};

template <class Surface>
constexpr BoundaryPlan<Surface> PlanBoundary(uint32_t passIndex, Surface rawAnswer, Surface prevPassInput,
                                             Surface clamp0, Surface clamp1)
{
    const uint32_t slot = (passIndex - 1u) % 2u;
    return BoundaryPlan<Surface> {
        rawAnswer,
        prevPassInput,
        slot == 0u ? clamp0 : clamp1,
        slot,
    };
}

inline void ArmSkippedPassResets(std::array<bool, PassConfig::MaxPasses>& passReset, uint32_t firstSkipped)
{
    for (uint32_t downstream = std::max(1u, firstSkipped); downstream < PassConfig::MaxPasses; ++downstream)
        passReset[downstream] = true;
}

namespace Reference
{
struct float3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    constexpr float3() = default;
    constexpr float3(float vx, float vy, float vz) : x(vx), y(vy), z(vz) {}
};

constexpr float3 operator+(float3 a, float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
constexpr float3 operator-(float3 a, float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
constexpr float3 operator*(float s, float3 v) { return { s * v.x, s * v.y, s * v.z }; }
constexpr float3 operator*(float3 v, float s) { return { v.x * s, v.y * s, v.z * s }; }

inline bool isfinite(float v) { return std::isfinite(v); }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline float3 min(float3 a, float3 b) { return { min(a.x, b.x), min(a.y, b.y), min(a.z, b.z) }; }
inline float3 max(float3 a, float3 b) { return { max(a.x, b.x), max(a.y, b.y), max(a.z, b.z) }; }
inline float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float3 clamp(float3 v, float3 lo, float3 hi)
{
    return { clamp(v.x, lo.x, hi.x), clamp(v.y, lo.y, hi.y), clamp(v.z, lo.z, hi.z) };
}
inline float saturate(float v) { return clamp(v, 0.0f, 1.0f); }

#define PC_FN inline
#define PC_UNROLL
#include "../shaders/dlssnr/precompile/dlssnr_pass_clamp_rule.h"
#undef PC_UNROLL
#undef PC_FN
} // namespace Reference
} // namespace DlssNr::PassClamp
