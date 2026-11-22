#pragma once

#include <dlssnr/DlssNr_Cadence.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

// Detail stability ([DlssNr] DetailStability, docs/dlssnr/design/detail-stability.md): a temporal filter on the model's edit in
// the log domain, history reprojected with the game's motion vectors, clipped to the current 3x3 and rejected where the input
// moved under it. The per-pixel rule is shaders/dlssnr/precompile/dlssnr_detail_stability_rule.h; this is its scheduling.
//
// Pure: flags, ids and times in, a decision out, so tests/nr-detail-stability pins it without a device.
namespace DlssNr::DetailStability
{
// The weight of the current frame per frame. NR Nexus ships 0.15: a history weight of 0.85 that brought every case it
// measured below DLSS alone. 1 is no filter; below kMinBlend the history would outlast what it is a history of.
constexpr float kDefaultBlend = 0.15f;
constexpr float kMinBlend = 0.05f;
constexpr float kMaxBlend = 1.0f;

// The configured blend as the pass uses it; anything not finite reads as the default.
inline float Blend(float configured)
{
    return std::isfinite(configured) ? std::clamp(configured, kMinBlend, kMaxBlend) : kDefaultBlend;
}

// The rejection band, in c / (1 + c) of the input's luminance: none below kBandLow, whole at kBandHigh. Near binary on
// purpose (A-ENTROPY: a wide smoothstep wobbles).
constexpr float kBandLow = 0.02f;
constexpr float kBandHigh = 0.10f;

enum class Reason : uint8_t
{
    None,
    Off,
    DriverProxy,    // the proxy returns before the resolve
    BeforeUpscale,  // the input is the jittered render-resolution frame: every pixel moves
    NoGameGuides,   // no motion to reproject with
    FrameGeneration, // generated frames are not frames of the model's history
    Cadence,        // the carried frames are not frames of the model's history
    NoPresentCount, // two views in one present cannot be told apart
    NoSurfaces,
};

struct Inputs
{
    bool enabled = false;
    bool driverProxy = false;
    bool beforeUpscale = false;
    bool gameGuides = false;
    bool frameGeneration = false;
    bool cadence = false; // a cadence above 1 is requested
    bool surfaces = true;
};

constexpr Reason Refusal(const Inputs& in)
{
    if (!in.enabled)
        return Reason::Off;
    if (in.driverProxy)
        return Reason::DriverProxy;
    if (in.beforeUpscale)
        return Reason::BeforeUpscale;
    if (!in.gameGuides)
        return Reason::NoGameGuides;
    if (in.frameGeneration)
        return Reason::FrameGeneration;
    if (in.cadence)
        return Reason::Cadence;
    if (!in.surfaces)
        return Reason::NoSurfaces;
    return Reason::None;
}

struct Decision
{
    bool run = false;
    bool historyValid = false; // there is a history of the frame before to filter with
    Reason reason = Reason::None;
};

// Whether the filter runs on this frame and whether its history is any use. The history is the last frame's filtered edit:
// it is dropped by a refusal, a second call in one present (another view), a gap longer than the cadence's, a change of the
// signature (size, format, route, the settings the edit depends on), a reset of the model, and a frame the pass did not
// record.
class Gate
{
    uint64_t _lastPresent = 0;
    int64_t _lastMs = 0;
    uint64_t _signature = 0;
    bool _seen = false;
    bool _ran = false;

  public:
    Decision Decide(const Inputs& in, uint64_t present, int64_t nowMs, uint64_t signature, bool resetPending)
    {
        Decision d;
        d.reason = Refusal(in);
        if (d.reason != Reason::None)
        {
            _ran = false;
            return d;
        }
        if (_seen && present != 0 && present == _lastPresent)
        {
            d.reason = Reason::NoPresentCount;
            _ran = false;
            return d;
        }

        const bool gap = _seen && nowMs - _lastMs > Cadence::kGapMs;
        d.run = true;
        d.historyValid = _seen && _ran && !gap && signature == _signature && !resetPending;

        _seen = true;
        _lastPresent = present;
        _lastMs = nowMs;
        _signature = signature;
        return d;
    }

    // What the pass did on the frame it was decided for.
    void Ran(bool filtered) { _ran = filtered; }

    bool Filtered() const { return _ran; }
    void Reset() { *this = Gate {}; }
};
} // namespace DlssNr::DetailStability
