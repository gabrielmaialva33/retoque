#pragma once

// Portable policy and reference math for de-jittering render-resolution motion vectors handed to DLSS-NR
// (docs/dlssnr/design/2026-11-22-dejitter-motion-vectors.md).
// Adapted from kkyleeb21's OptiScaler_DLSSNR D18 research branch (nr_jitter_policy.h / NativeControl.h, GPL-3.0).

#include <cmath>
#include <cstdint>

namespace DlssNr::Dejitter
{
inline constexpr float kMinMvScale = 1e-6f;

enum class Route : uint8_t
{
    AfterUpscale = 0,
    Present = 1,
    NativeDx11Scene = 2,
};

struct Inputs
{
    bool enabled = false;
    bool beforeUpscale = false;
    bool held = false;
    bool gameGuides = false;
    bool mvJittered = false;
    bool mvLowResolution = false;
    bool haveJitter = false;
    bool reset = false;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t motionWidth = 0;
    uint32_t motionHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;
    Route route = Route::AfterUpscale;
    uint64_t pictureEpoch = 0;
    uint64_t renderSequence = 0;
};

struct State
{
    bool active = false;
    bool hasHistory = false;
    float prevJitterX = 0.0f;
    float prevJitterY = 0.0f;
    float prevMvScaleX = 1.0f;
    float prevMvScaleY = 1.0f;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    Route route = Route::AfterUpscale;
    uint64_t pictureEpoch = 0;
    uint64_t prevRenderSequence = 0;

    void Reset() { *this = State {}; }
};

struct Decision
{
    bool active = false;
    bool dispatch = false;
    bool pulseReset = false;
    float deltaMvX = 0.0f;
    float deltaMvY = 0.0f;
};

inline bool ValidScale(float scale) { return std::isfinite(scale) && std::abs(scale) > kMinMvScale; }

inline bool Eligible(const Inputs& in)
{
    return in.enabled && !in.beforeUpscale && !in.held && in.gameGuides && in.mvJittered && in.mvLowResolution &&
           in.haveJitter && std::isfinite(in.jitterX) && std::isfinite(in.jitterY) && ValidScale(in.mvScaleX) &&
           ValidScale(in.mvScaleY) && in.renderWidth > 0 && in.renderHeight > 0 && in.outputWidth > 0 &&
           in.outputHeight > 0 && in.motionWidth == in.renderWidth && in.motionHeight == in.renderHeight &&
           in.renderSequence != 0;
}

inline Decision Step(State& state, const Inputs& in)
{
    if (!Eligible(in))
    {
        const bool wasActive = state.active;
        state.Reset();
        return Decision {
            false, false, wasActive && !in.held, 0.0f, 0.0f,
        };
    }

    const bool contractChanged = !state.active || !state.hasHistory || state.renderWidth != in.renderWidth ||
                                 state.renderHeight != in.renderHeight || state.outputWidth != in.outputWidth ||
                                 state.outputHeight != in.outputHeight || state.route != in.route ||
                                 state.pictureEpoch != in.pictureEpoch ||
                                 std::abs(state.prevMvScaleX - in.mvScaleX) > kMinMvScale ||
                                 std::abs(state.prevMvScaleY - in.mvScaleY) > kMinMvScale;
    const bool sequenceBroken = in.reset || in.renderSequence != state.prevRenderSequence + 1;

    if (contractChanged || sequenceBroken)
    {
        state.active = true;
        state.hasHistory = true;
        state.prevJitterX = in.jitterX;
        state.prevJitterY = in.jitterY;
        state.prevMvScaleX = in.mvScaleX;
        state.prevMvScaleY = in.mvScaleY;
        state.renderWidth = in.renderWidth;
        state.renderHeight = in.renderHeight;
        state.outputWidth = in.outputWidth;
        state.outputHeight = in.outputHeight;
        state.route = in.route;
        state.pictureEpoch = in.pictureEpoch;
        state.prevRenderSequence = in.renderSequence;
        return Decision {
            true, false, true, 0.0f, 0.0f,
        };
    }

    const float deltaMvX = (in.jitterX - state.prevJitterX) / in.mvScaleX;
    const float deltaMvY = (in.jitterY - state.prevJitterY) / in.mvScaleY;

    state.prevJitterX = in.jitterX;
    state.prevJitterY = in.jitterY;
    state.prevRenderSequence = in.renderSequence;

    if (!std::isfinite(deltaMvX) || !std::isfinite(deltaMvY))
    {
        return Decision {
            true, false, true, 0.0f, 0.0f,
        };
    }

    const bool nonZero = deltaMvX != 0.0f || deltaMvY != 0.0f;
    return Decision {
        true, nonZero, false, deltaMvX, deltaMvY,
    };
}

namespace Reference
{
struct float2
{
    float x = 0.0f;
    float y = 0.0f;
    constexpr float2() = default;
    constexpr float2(float vx, float vy) : x(vx), y(vy) {}
};

inline bool isfinite(float v) { return std::isfinite(v); }

#define DJ_FN inline
#include "../shaders/dlssnr/precompile/dlssnr_dejitter_rule.h"
#undef DJ_FN
} // namespace Reference
} // namespace DlssNr::Dejitter
