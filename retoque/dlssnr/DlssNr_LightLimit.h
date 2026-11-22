#pragma once

#include <dlssnr/DlssNr_Cadence.h>

#include <cstdint>

// The light limiter at any cadence ([DlssNr] LightLimiter, docs/dlssnr/design/light-limiter.md).
//
// Model cadence's smoothing owns a light limiter that holds the mean light of what the cadence shows to 1.5 stops a
// second while the frame's own light holds still (DlssNr_Cadence.h). It only ever ran with Cadence above 1 and
// CadenceSmoothing on: a default single pass on every frame had no light stabilization at all. This is the same limiter
// on the answer of any frame the cadence's own does not hold, in two forms:
//
// - Global: the cadence's own rule on the whole picture's light, 1.5 stops a second (the same constants).
// - Regional (Skynizz's anti light pop-in, optiscaler-dlss5 v1.2.1, GPL-3.0): the rule once per tile of kRegionTile
//   pixels, 0.75 stops a second, where the tile's own light held still; the gain is bilinear between tile centres, so
//   what moves is the regional band a tile wide and what is finer -- the detail -- keeps the answer's own light.
//
// Pure: flags, ids and times in, a decision out, so tests/nr-light-limiter pins it without a device.
namespace DlssNr::LightLimit
{
enum class Mode : uint32_t
{
    Off = 0,
    Global = 1,
    Regional = 2,
};

// The configured value as the pass uses it. Anything but 1 and 2 -- 0, a hand-edited 7 -- reads as off, so a typo never
// turns on an option that changes what is shown.
constexpr Mode Requested(uint32_t configured)
{
    return configured == 1 ? Mode::Global : configured == 2 ? Mode::Regional : Mode::Off;
}

// The regional limiter's numbers, fixed as the global one's are. Skynizz: tent about 36 px, 0.15 stop of the region's own
// light as the gate, 0.75 stop/s, released as the step nears 0.35 stop and by motion over about 6 px a frame.
constexpr uint32_t kRegionTile = 32;       // working pixels per tile edge; the band is one tile wide
constexpr float kRegionRate = 0.75f;       // stops per second
constexpr float kRegionReleaseLow = 0.30f; // drift between the answer and what was shown, in stops, where the hold starts to let go
constexpr float kRegionReleaseHigh = 0.40f; // ... and has let go
constexpr float kRegionMotionPixels = 6.0f; // mean motion of a tile, in pixels of a frame, at which it is not held

constexpr uint32_t TilesFor(uint32_t extent) { return (extent + kRegionTile - 1) / kRegionTile; }

// The regional limiter's step, in stops, for a frame that took `seconds`: clamped as the global one is, so a hitch
// cannot pass a whole re-grade and a frame time of nothing does not freeze the light.
constexpr float RegionStep(float seconds)
{
    const float t = !(seconds >= Cadence::kLightMinSeconds) ? Cadence::kLightMinSeconds
                    : seconds > Cadence::kLightMaxSeconds   ? Cadence::kLightMaxSeconds
                                                            : seconds;
    return kRegionRate * t;
}

// A mean motion in the pixels of the frame's own size, in working pixels.
constexpr float RegionMotionPixels(float scale) { return kRegionMotionPixels * (scale > 0.0f ? scale : 1.0f); }

struct Decision
{
    bool run = false;   // limit this frame's answer
    bool reset = false; // ... with nothing before it to hold it to
    Mode mode = Mode::Off;
};

// Whether the limiter runs on this frame, and whether it starts over. State is the frame before: it was limited, by
// this mode, at this size, within kGapMs. The cadence's limiter keeps its own state; a frame that one limited is a
// frame this one did not, so this one starts over on the next frame it runs.
class Gate
{
    uint64_t _lastPresent = 0;
    int64_t _lastMs = 0;
    uint64_t _signature = 0;
    Mode _lastMode = Mode::Off;
    bool _seen = false;
    bool _limited = false; // the last frame the gate was asked about was limited

  public:
    // otherLimiter: the cadence's own limiter held this frame (smoothing on a running cadence). resetPending: the model's
    // history resets this frame -- the game's reset, a tuning pulse -- so what it says now is a new decision.
    Decision Decide(Mode mode, bool otherLimiter, uint64_t present, int64_t nowMs, uint64_t signature, bool resetPending)
    {
        Decision d;
        d.mode = mode;
        if (mode == Mode::Off || otherLimiter)
        {
            _limited = false;
            return d;
        }
        // A second call in one present is another view: one view's light must not hold another's.
        if (_seen && present != 0 && present == _lastPresent)
        {
            _limited = false;
            return d;
        }

        const bool gap = _seen && nowMs - _lastMs > Cadence::kGapMs;
        d.run = true;
        d.reset = !_seen || !_limited || gap || mode != _lastMode || signature != _signature || resetPending;

        _seen = true;
        _lastPresent = present;
        _lastMs = nowMs;
        _signature = signature;
        _lastMode = mode;
        return d;
    }

    // What the limiter did on the frame it was decided for.
    void Ran(bool limited) { _limited = limited; }

    bool Limited() const { return _limited; }
    void Reset() { *this = Gate {}; }
};
} // namespace DlssNr::LightLimit
