#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// The exposure governor ([DlssNr] ExposureGovernor, docs/dlssnr/design/exposure-governor.md): the white point the pass
// APPLIES for WhitePointSource=1 moves at a bounded rate toward the one the game's exposure implies.
//
// The estimate is never touched: it is the game's own, fresh every frame. Only the applied divisor is limited, so a
// frozen estimate is not representable and a single-frame step is not either. The rule is DLSS5-NR-Nexus's
// normalization governor (williamstosh, MIT, shaders/v6_autoscale.cs_5_1.hlsl at 8fabd388e9: asymmetric wall-clock
// limits, a hysteresis hold, a hold on a frame that cannot be read, a re-prime on the game's reset, one state per
// stream); Licenses/NRNexus_LICENSE.txt carries the licence. The code is ours: Nexus keeps the state in a 1x1 UAV
// beside a frame histogram, here it is a few floats on the host beside the game's exposure.
//
// Pure: values and seconds in, a value out, so tests/nr-exposure-governor pins it without a device.
namespace DlssNr::ExposureGovernor
{
// Stops per second. The white point RISING is the picture brightening under the model (the over-range direction): fast.
// Falling is the scene relaxing: slow. NR Nexus ships 4 and 0.5.
constexpr float kAttackStopsPerSecond = 4.0f;
constexpr float kReleaseStopsPerSecond = 0.5f;

// The hysteresis hold: the applied value moves only once the target has left it by kEngageStops, then tracks until
// within kSettleStops. NR Nexus ships 0.15 and 0.02.
constexpr float kEngageStops = 0.15f;
constexpr float kSettleStops = 0.02f;

// A step is never given more time than this: a hitch must not let a whole re-grade through in one frame. A pause
// longer than kGapSeconds (the menu, a loading screen, the pass off) says the held value describes another scene.
constexpr float kMaxStepSeconds = 0.25f;
constexpr float kGapSeconds = 2.0f;

// A reading the white point is not governed from. The exposure a black frame, a loading screen or a fade to black
// publishes is an extreme one, and holding through it is what NR Nexus does with a frame it cannot read. This fork
// measures no frame light on this path, so the signature is the game's own exposure: PlausibilityLatch's range
// (DlssNr_Exposure.h), one end at a time, which that latch documents as what a black frame can do. Kept as numbers here
// so the header stands alone; tests/nr-exposure-governor checks they equal the latch's.
constexpr float kPlausibleMinImpliedWhite = 0.02f;
constexpr float kPlausibleMaxImpliedWhite = 512.0f;

constexpr bool PlausibleImpliedWhite(float implied)
{
    return implied >= kPlausibleMinImpliedWhite && implied <= kPlausibleMaxImpliedWhite;
}

// What one reading did, for the log and the tests.
enum class Outcome : uint8_t
{
    Passed,   // not governed: nothing valid to govern, or the target was taken as it is
    Primed,   // the first reading after a start or a re-prime: taken as it is
    Held,     // inside the hysteresis band, or a reading that cannot be read: the applied value stayed
    Limited,  // moved toward the target, by less than the whole way
    Followed, // moved the whole way: the limit did not bite
};

struct Result
{
    float applied = 0.0f;
    Outcome outcome = Outcome::Passed;
};

class State
{
    float _applied = 0.0f; // 0 is unprimed
    bool _tracking = false;
    bool _pending = false; // a re-prime asked for on a frame that could not be read: taken on the first one that can

  public:
    void Reset() { *this = State {}; }

    bool Primed() const { return _applied > 0.0f; }
    float Applied() const { return _applied; }
    bool Tracking() const { return _tracking; }
    bool Pending() const { return _pending; }

    // One reading. target: the white point the game's exposure implies (finite, above zero, or it passes untouched).
    // seconds: the time since the last reading. unreadable: a frame that must not move it (see
    // PlausibleImpliedWhite). reprime: the game's reset or a scene cut, the applied value is taken from the target now.
    Result Update(float target, float seconds, bool unreadable, bool reprime)
    {
        if (!std::isfinite(target) || !(target > 0.0f))
            return { target, Outcome::Passed };

        const float dt = !(seconds >= 0.0f) ? 0.0f : seconds;
        const bool gap = dt > kGapSeconds;
        const bool requested = reprime || gap || _pending;

        if (unreadable)
        {
            // Hold, keeping the hysteresis state. A start or a re-prime waits for the first reading that can be read.
            if (Primed())
            {
                _pending = requested;
                return { _applied, Outcome::Held };
            }
            _pending = true;
            return { target, Outcome::Passed };
        }

        if (requested || !Primed())
        {
            _applied = target;
            _tracking = false;
            _pending = false;
            return { _applied, Outcome::Primed };
        }

        const float error = std::log2(target / _applied);
        _tracking = std::fabs(error) > (_tracking ? kSettleStops : kEngageStops);
        if (!_tracking)
            return { _applied, Outcome::Held };

        const float step = std::min(dt, kMaxStepSeconds);
        const float stops = std::clamp(error, -kReleaseStopsPerSecond * step, kAttackStopsPerSecond * step);
        _applied *= std::exp2(stops);
        return { _applied, stops == error ? Outcome::Followed : Outcome::Limited };
    }
};
} // namespace DlssNr::ExposureGovernor
