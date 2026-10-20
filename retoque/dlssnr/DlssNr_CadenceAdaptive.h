#pragma once

#include "DlssNr_Cadence.h"
#include <algorithm>
#include <array>
#include <cmath>

// Sparse, delayed GPU feedback. Policy only; no device, clocks, config or global state.
// See docs/dlssnr/design/cadence-adaptive.md for measured scope and conservative thresholds.
namespace DlssNr::Cadence::Adaptive
{
constexpr uint32_t Grid = 16;
struct Pixel
{
    float motion = 0, rejection = 0, valid = 0, unused = 0;
};
struct Sample
{
    uint64_t present = 0;
    int64_t ms = 0;
    float motion = 0;
    float rejection = 0;
    bool valid = false;
};
inline Sample Summarize(const std::array<Pixel, Grid * Grid>& pixels, uint64_t present, int64_t ms)
{
    Sample result { present, ms };
    std::array<float, Grid * Grid> motion {};
    size_t count = 0;
    float rejected = 0;
    for (const auto& p : pixels)
    {
        if (!std::isfinite(p.valid) || p.valid < 0)
            return result;
        if (p.valid == 0)
            continue;
        if (!std::isfinite(p.motion) || p.motion < 0 || !std::isfinite(p.rejection) || p.rejection < 0 ||
            p.rejection > 1)
            return result;
        motion[count++] = p.motion;
        rejected += p.rejection;
    }
    if (count < 8)
        return result;
    std::sort(motion.begin(), motion.begin() + count);
    result.motion = motion[(count * 9 + 9) / 10 - 1];
    result.rejection = rejected / static_cast<float>(count);
    result.valid = true;
    return result;
}
class Controller
{
    Sample _last {};
    int64_t _calmSince = -1;
    uint32_t _interval = 1;
    uint32_t _candidate = 1;

    static bool Fresh(const Sample& s, uint64_t present, int64_t ms)
    {
        return s.valid && s.present < present && present - s.present <= 4 && ms >= s.ms && ms - s.ms < kGapMs &&
               std::isfinite(s.motion) && s.motion >= 0 && std::isfinite(s.rejection) && s.rejection >= 0 &&
               s.rejection <= 1;
    }

  public:
    void Reset() { *this = {}; }
    void Observe(const Sample& s, uint64_t present, int64_t ms)
    {
        if (s.present <= _last.present)
            return;
        if (!Fresh(s, present, ms))
        {
            Reset();
            _last = s;
            _last.valid = false;
            return;
        }
        if (!Fresh(_last, present, ms))
        {
            _calmSince = -1;
            _interval = 1;
        }
        _last = s;
        const uint32_t target = s.motion >= 1.0f || s.rejection >= 0.15f ? 1 : s.motion >= 0.25f ? 2 : kMaxCadence;
        if (target < _interval)
        {
            _interval = target;
            _calmSince = -1;
        }
        else if (target > _interval)
        {
            if (_calmSince < 0 || _candidate != target)
                _calmSince = s.ms;
            _candidate = target;
            if (s.ms - _calmSince >= 300)
            {
                _interval = target;
                _calmSince = -1;
            }
        }
        else
            _calmSince = -1;
    }
    uint32_t Interval(uint32_t maximum, uint64_t present, int64_t ms) const
    {
        return Fresh(_last, present, ms) ? std::min(Requested(maximum), _interval) : 1;
    }
};
} // namespace DlssNr::Cadence::Adaptive
