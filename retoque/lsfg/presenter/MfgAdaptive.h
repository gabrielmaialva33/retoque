#pragma once

// Adaptive LSFG multiplier, phase 1: a log-only shadow adviser for the MFG presenter (MfgSwapChain_Dx12.cpp,
// docs/dlssnr/design/lsfg-adaptive-shadow.md). It watches the real frames the presenter thread presents and says, in
// the 10 s summary, what an adaptive policy would pick. Nothing here feeds setGeneratedFrames or the pacing
// (tests/lsfg-adaptive holds that in the source). Header-only and free of <windows.h>, so the host tier compiles it.
//
// RefreshBudget, CadenceEstimator, DeliveryWindow and HigherLoadIsUseful reimplement algorithms of MAKO
// (https://github.com/eugeniosegala/MAKO, Copyright (C) 2025-2026 Eugenio Segala and contributors, GPL-3.0-or-later),
// read at commit a321c2a: FixedRefreshBudget (engine/mako-render/src/presentation_policy.hpp), the cadence observation
// of AdaptiveScheduler (adaptive_scheduler.cpp), GeneratedDeliveryWindow (generated_frame_delivery.hpp) and
// adaptiveHigherLoadIsUseful (adaptive_scheduler.cpp). No MAKO code is copied; the constants are MAKO's.
// Licenses/MAKO_ATTRIBUTION.txt. The target is XeFG Dynamic MFG's own (XeFG_DynamicTargetFps).

#include <framegen/xefg/XeFG_DynamicCount.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace LsfgMfg::Adaptive
{

// x2..x4: 1..3 generated frames per real frame (MfgPacing.h kMaxGeneratedFrames).
constexpr uint32_t kMinGenerated = 1;
constexpr uint32_t kMaxGenerated = 3;

// MAKO's constants.
constexpr double kBudgetGapSec = 0.25;         // a longer real-frame gap resets the refresh budget
constexpr double kIntervalSmoothing = 0.25;    // interval EMA weight of the new sample
constexpr double kBurstBaselineRatio = 3.0;    // a burst is faster than 3x the baseline real rate...
constexpr double kBurstTargetRatio = 2.0;      // ...and than 2x the target
constexpr double kMinimumBaseFps = 10.0;       // under it, a stall
constexpr double kDropRatio = 2.0;             // an interval 2x the average is a drop candidate
constexpr uint32_t kDropConfirmFrames = 3;     // three in a row confirm a drop
constexpr uint64_t kDeliveryToleranceDiv = 20; // 5% of requested frames may miss, integer
constexpr double kTargetSatisfiedRatio = 0.98; // the target counts as met from 98%
constexpr double kMinimumNetGainRatio = 0.02;  // a higher load must net 2% of the target (x3 and up)

// Ours, not MAKO's.
constexpr double kRefreshRetrySec = 2.0;    // XeFG_DynamicMFG::DisplayRefreshFps re-reads the refresh every 2 s
constexpr double kEvidenceMaxAgeSec = 60.0; // a measurement at another multiplier is a different scene after this

// MAKO FixedRefreshBudget::plan: how many of the generated frames the display refresh can show. The real-frame interval
// is averaged (0.75 / 0.25), every real frame adds max(interval x refresh, 1) outputs of credit, the frame generates
// min(floor(credit) - 1, max) and spends them with itself; at most one output of credit carries over, so a slow
// source never banks a backlog to overshoot with. The first frame, a clock going backwards and a gap over 250 ms
// generate nothing and start over. Refresh unknown: the maximum.
class RefreshBudget
{
  public:
    uint32_t Plan(double nowSec, uint32_t refreshHz, uint32_t maxGenerated)
    {
        if (refreshHz == 0 || maxGenerated == 0)
        {
            _last = nowSec;
            _hasLast = true;
            return maxGenerated;
        }

        if (!_hasLast)
        {
            _last = nowSec;
            _hasLast = true;
            return 0;
        }

        const double interval = nowSec - _last;
        _last = nowSec;
        if (interval <= 0.0 || interval > kBudgetGapSec)
        {
            _smoothed = 0.0;
            _credit = 0.0;
            return 0;
        }

        _smoothed =
            _smoothed == 0.0 ? interval : _smoothed * (1.0 - kIntervalSmoothing) + interval * kIntervalSmoothing;

        // Both sides of the timestamp jitter count, early and late intervals alike.
        _credit += std::max(_smoothed * double(refreshHz), 1.0);
        const uint64_t requested = std::max<uint64_t>(1, uint64_t(std::floor(_credit + 1e-9)));
        const uint32_t generated = uint32_t(std::min<uint64_t>(requested - 1, maxGenerated));
        _credit = std::min(std::max(_credit - double(generated + 1), 0.0), 1.0);
        return generated;
    }

    void Reset() { *this = {}; }

    double Credit() const { return _credit; }

  private:
    double _last = 0.0;
    bool _hasLast = false;
    double _smoothed = 0.0;
    double _credit = 0.0;
};

enum class Cadence
{
    First,       // no interval yet
    Sample,      // averaged
    Burst,       // faster than the burst threshold: excluded
    Stall,       // under 10 fps or not after the last frame: excluded
    DropPending, // 2x the average or more, not yet confirmed: held in a shadow estimate
    Drop,        // confirmed: the average rebased at the shadow estimate
};

struct CadenceCounts
{
    uint64_t samples = 0;
    uint64_t bursts = 0;
    uint64_t stalls = 0;
    uint64_t hitches = 0; // drop candidates that did not last three frames
    uint64_t drops = 0;   // confirmed cadence drops
};

// The real frame rate as MAKO's AdaptiveScheduler observes it (adaptive_scheduler.cpp, observeCadence): an interval
// EMA that excludes transients faster than max(3x the baseline, 2x the target) and stalls under 10 fps, and confirms
// a cadence drop (intervals 2x the average) only on the third in a row. While a drop is unconfirmed the average is
// kept and the slow samples go to a shadow estimate; a run that ends early adopts that estimate (an isolated hitch
// moves the average a little, not 2x), a confirmed one rebases at it. MAKO then stabilises for 1 s before it plans
// again; the shadow only counts the drop.
class CadenceEstimator
{
  public:
    Cadence Observe(double nowSec, double targetFps)
    {
        if (!_hasLast)
        {
            _last = nowSec;
            _hasLast = true;
            return Cadence::First;
        }

        const double interval = nowSec - _last;
        _last = nowSec;

        if (interval <= 0.0)
            return Stall();

        const double baselineFps = _smoothed > 0.0 ? 1.0 / _smoothed : 0.0;
        const double threshold =
            std::max(baselineFps * kBurstBaselineRatio, std::max(targetFps, 0.0) * kBurstTargetRatio);
        if (threshold > 0.0 && 1.0 / interval > threshold)
        {
            _counts.bursts++;
            _dropFrames = 0;
            return Cadence::Burst;
        }

        if (interval > 1.0 / kMinimumBaseFps)
            return Stall();

        const bool candidate = _smoothed > 0.0 && interval >= _smoothed * kDropRatio;
        if (candidate)
        {
            if (_dropFrames == 0)
                _pending = _smoothed;
            _pending = (1.0 - kIntervalSmoothing) * _pending + kIntervalSmoothing * interval;
            _dropFrames++;
        }
        else
        {
            if (_dropFrames > 0)
            {
                _smoothed = _pending;
                _counts.hitches++;
            }
            _dropFrames = 0;
        }

        if (_dropFrames >= kDropConfirmFrames)
        {
            _smoothed = _pending;
            _dropFrames = 0;
            _counts.drops++;
            return Cadence::Drop;
        }

        if (candidate)
            return Cadence::DropPending;

        _smoothed =
            _smoothed == 0.0 ? interval : (1.0 - kIntervalSmoothing) * _smoothed + kIntervalSmoothing * interval;
        _counts.samples++;
        return Cadence::Sample;
    }

    // The next interval starts here without this one being a sample (an entry dropped before presentation hides how
    // many real frames it spans).
    void Rebase(double nowSec)
    {
        _last = nowSec;
        _hasLast = true;
        _dropFrames = 0;
    }

    double BaseFps() const { return _smoothed > 0.0 ? 1.0 / _smoothed : 0.0; }
    const CadenceCounts& Counts() const { return _counts; }
    void Reset() { *this = {}; }

  private:
    Cadence Stall()
    {
        _counts.stalls++;
        _dropFrames = 0;
        return Cadence::Stall;
    }

    double _last = 0.0;
    bool _hasLast = false;
    double _smoothed = 0.0;
    double _pending = 0.0;
    uint32_t _dropFrames = 0;
    CadenceCounts _counts;
};

// MAKO GeneratedDeliveryWindow: healthy while at most 5% of the requested generated frames missed presentation,
// integer, so under 20 requested every one must be delivered.
class DeliveryWindow
{
  public:
    void Record(uint64_t requested, uint64_t accepted)
    {
        _requested += requested;
        _accepted += std::min(requested, accepted);
    }

    bool Healthy() const { return _requested - _accepted <= _requested / kDeliveryToleranceDiv; }
    uint64_t Requested() const { return _requested; }
    uint64_t Accepted() const { return _accepted; }
    void Reset() { *this = {}; }

  private:
    uint64_t _requested = 0;
    uint64_t _accepted = 0;
};

// A load (generated frames per real frame) as MAKO compares them: the real rate and the output it gives, capped at
// the target (frames past it buy nothing).
struct LoadOutcome
{
    double baseFps = 0.0;
    double outputFps = 0.0;
};

inline LoadOutcome OutcomeAt(double targetFps, uint32_t generated, double baseFps)
{
    return { baseFps, std::min(targetFps, baseFps * double(generated + 1)) };
}

// MAKO adaptiveHigherLoadIsUseful: a higher load must add more output than the real frames it costs, plus 2% of the
// target from x3 up. Only a delivered (not projected) reach of the target, from under it, waives the real-frame cost.
inline bool HigherLoadIsUseful(double targetFps, uint32_t testedGenerated, LoadOutcome lower, LoadOutcome higher,
                               bool targetDeliveryQualified)
{
    const double gain = higher.outputFps - lower.outputFps;
    const double targetFloor = targetFps * kTargetSatisfiedRatio;
    const bool reachesTarget = testedGenerated > 1 && targetDeliveryQualified && lower.outputFps < targetFloor &&
                               higher.outputFps >= targetFloor;
    const double cost = reachesTarget ? 0.0 : std::max(0.0, lower.baseFps - higher.baseFps);
    const double scale = std::max({ 1.0, lower.baseFps, higher.baseFps, lower.outputFps, higher.outputFps });
    const double tolerance = std::numeric_limits<double>::epsilon() * scale * 8.0;
    const double margin = testedGenerated > 1 ? targetFps * kMinimumNetGainRatio : 0.0;
    return gain + tolerance >= cost + margin;
}

// The presented rate an adaptive multiplier would aim for: XeFG Dynamic MFG's auto target (no FramerateTargetDMFG of
// LSFG's own), held to the FPS limit, and to the refresh rate under V-Sync. 0: none (refresh unknown, no limit).
inline double TargetFps(double limitFps, double refreshHz, bool vsync)
{
    return double(XeFG_DynamicTargetFps(0.0f, float(limitFps), float(refreshHz), vsync));
}

enum class Why
{
    NoTarget,          // refresh unknown and no FPS limit
    NoCadence,         // no real frame rate yet
    Nearest,           // XeFG: the presented rate nearest the target
    DeliveryPressure,  // MAKO: over 5% of generated frames missed presentation
    CurrentDoesNotPay, // MAKO: measured one step less gives as much, net of the real frames this one costs
    TargetMet,         // MAKO: the target is met, no higher load to try
    HigherPays,        // MAKO: one step more, measured, pays for its real frames
    HigherDoesNotPay,  // MAKO: one step more, measured, does not
    HigherUntested,    // MAKO: one step more was never measured here; only a trial could tell
    AtMost,            // MAKO: x4 already, short of the target
    Count
};

inline const char* WhyText(Why why)
{
    switch (why)
    {
    case Why::NoTarget:
        return "no target";
    case Why::NoCadence:
        return "no cadence";
    case Why::Nearest:
        return "nearest the target";
    case Why::DeliveryPressure:
        return "delivery pressure";
    case Why::CurrentDoesNotPay:
        return "one step less measured as good";
    case Why::TargetMet:
        return "target met";
    case Why::HigherPays:
        return "one step more measured, pays";
    case Why::HigherDoesNotPay:
        return "one step more measured, does not pay";
    case Why::HigherUntested:
        return "one step more untested";
    case Why::AtMost:
        return "at x4";
    default:
        return "?";
    }
}

struct AdviceInput
{
    double baseFps = 0.0;   // the cadence estimator's real frame rate at the current count
    double refreshHz = 0.0; // 0 unknown
    double limitFps = 0.0;  // OptiScaler's FPS limit, 0 none
    bool vsync = false;
    uint32_t currentGenerated = 1;
    bool deliveryHealthy = true;
    double evidenceBaseFps[kMaxGenerated + 1] {}; // real rate measured at each generated count (index), 0 none
};

struct Advice
{
    double targetFps = 0.0;
    uint32_t xefgGenerated = 1;
    Why xefgWhy = Why::NoTarget;
    uint32_t makoGenerated = 1;
    Why makoWhy = Why::NoTarget;
};

// What two policies would pick at this moment, generated frames 1..3 (x2..x4); the caller applies neither.
// - XeFG-style: the count whose presented rate, (n + 1) x the real rate, lands nearest the target. XeFG_DynamicCount's
//   rounding "around" in one shot: no hysteresis, no learned frame cost, no checks (those need the count applied).
// - MAKO's acceptance rule over measured loads only: the current count against the real rate measured at one step
//   less and one step more. A step is advised only on measured evidence; without it the reason says a trial is needed.
inline Advice Advise(const AdviceInput& in)
{
    Advice a;
    const uint32_t current = std::clamp(in.currentGenerated, kMinGenerated, kMaxGenerated);
    a.targetFps = TargetFps(in.limitFps, in.refreshHz, in.vsync);
    a.xefgGenerated = a.makoGenerated = current;

    if (a.targetFps <= 0.0)
        return a; // NoTarget, both

    if (in.baseFps <= 0.0)
    {
        a.xefgWhy = a.makoWhy = Why::NoCadence;
        return a;
    }

    // XeFG-style, ties to fewer frames.
    double best = std::numeric_limits<double>::max();
    for (uint32_t g = kMinGenerated; g <= kMaxGenerated; ++g)
    {
        const double miss = std::abs(in.baseFps * double(g + 1) / a.targetFps - 1.0);
        if (miss < best)
        {
            best = miss;
            a.xefgGenerated = g;
        }
    }
    a.xefgWhy = Why::Nearest;

    // MAKO's rule.
    const LoadOutcome now = OutcomeAt(a.targetFps, current, in.baseFps);

    if (!in.deliveryHealthy)
    {
        a.makoGenerated = std::max(current - 1, kMinGenerated);
        a.makoWhy = Why::DeliveryPressure;
        return a;
    }

    if (current > kMinGenerated && in.evidenceBaseFps[current - 1] > 0.0 &&
        !HigherLoadIsUseful(a.targetFps, current, OutcomeAt(a.targetFps, current - 1, in.evidenceBaseFps[current - 1]),
                            now, false))
    {
        a.makoGenerated = current - 1;
        a.makoWhy = Why::CurrentDoesNotPay;
        return a;
    }

    if (now.outputFps >= a.targetFps * kTargetSatisfiedRatio)
    {
        a.makoWhy = Why::TargetMet;
        return a;
    }

    if (current >= kMaxGenerated)
    {
        a.makoWhy = Why::AtMost;
        return a;
    }

    const double higherFps = in.evidenceBaseFps[current + 1];
    if (higherFps <= 0.0)
    {
        a.makoWhy = Why::HigherUntested;
        return a;
    }

    if (HigherLoadIsUseful(a.targetFps, current + 1, now, OutcomeAt(a.targetFps, current + 1, higherFps), false))
    {
        a.makoGenerated = current + 1;
        a.makoWhy = Why::HigherPays;
    }
    else
    {
        a.makoWhy = Why::HigherDoesNotPay;
    }
    return a;
}

// One 10 s summary window of the shadow.
struct WindowReport
{
    uint32_t refreshHz = 0;
    bool vsync = false;
    double baseFps = 0.0;
    CadenceCounts cadence; // within the window
    uint64_t real = 0;
    uint64_t generated = 0;
    uint64_t budgeted = 0; // of `generated`, what the refresh budget would have generated
    uint64_t requested = 0;
    uint64_t accepted = 0;
    bool healthy = true;
    uint32_t current = 1; // the last entry's generated count
    bool steady = false;  // one count, no drop, no stall, healthy: recorded as evidence for that count
    Advice advice;
};

// The presenter thread's shadow: fed once per presented entry, after its presents, and closed by the 10 s summary.
class Shadow
{
  public:
    // handoffSec: when the game handed the entry's real frame over; base / framesToPresent: the entry's present
    // indices (a gap from the last entry is entries dropped before presentation); generated: frames it carried.
    void OnEntry(double handoffSec, uint64_t base, uint32_t framesToPresent, uint32_t generated, bool vsync)
    {
        const uint64_t skipped = _hasBase && base > _nextBase ? base - _nextBase : 0;
        _nextBase = base + framesToPresent;
        _hasBase = true;

        if (skipped > 0)
            _cadence.Rebase(handoffSec);
        else
            _cadence.Observe(handoffSec, TargetFps(_limitFps, double(_refreshHz), vsync));

        const uint32_t g = std::min(generated, kMaxGenerated);
        _budgeted += _budget.Plan(handoffSec, _refreshHz, g);
        _delivery.Record(g + skipped, g);
        _real++;
        _generated += g;
        _byGenerated[g]++;
        _current = std::max(g, kMinGenerated);
        _vsync = vsync;
    }

    // Due at once, again every 2 s while unknown, and otherwise at a summary 2 s after the last read.
    bool RefreshDue(double nowSec, bool summary) const
    {
        if (!_refreshSampled)
            return true;
        const bool aged = nowSec - _refreshSampledSec >= kRefreshRetrySec;
        return aged && (_refreshHz == 0 || summary);
    }

    void SetRefresh(double nowSec, int refreshHz)
    {
        _refreshSampled = true;
        _refreshSampledSec = nowSec;
        _refreshHz = refreshHz > 1 ? uint32_t(refreshHz) : 0; // 0 and 1 are "unspecified" (Util::GetActiveRefreshRate)
    }

    // The window's report and what the policies advise; records it as evidence when steady and starts a new window.
    WindowReport CloseWindow(double nowSec, double limitFps)
    {
        _limitFps = limitFps;

        WindowReport r;
        r.refreshHz = _refreshHz;
        r.vsync = _vsync;
        r.baseFps = _cadence.BaseFps();
        const CadenceCounts& c = _cadence.Counts();
        r.cadence = { c.samples - _start.samples, c.bursts - _start.bursts, c.stalls - _start.stalls,
                      c.hitches - _start.hitches, c.drops - _start.drops };
        r.real = _real;
        r.generated = _generated;
        r.budgeted = _budgeted;
        r.requested = _delivery.Requested();
        r.accepted = _delivery.Accepted();
        r.healthy = _delivery.Healthy();
        r.current = _current;
        r.steady = _real > 0 && _byGenerated[_current] == _real && r.cadence.drops == 0 && r.cadence.stalls == 0 &&
                   r.healthy && r.baseFps > 0.0;

        AdviceInput in;
        in.baseFps = r.baseFps;
        in.refreshHz = double(_refreshHz);
        in.limitFps = limitFps;
        in.vsync = _vsync;
        in.currentGenerated = _current;
        in.deliveryHealthy = r.healthy;
        for (uint32_t g = kMinGenerated; g <= kMaxGenerated; ++g)
            if (g != _current && _evidence[g].fps > 0.0 && nowSec - _evidence[g].atSec <= kEvidenceMaxAgeSec)
                in.evidenceBaseFps[g] = _evidence[g].fps;
        r.advice = Advise(in);

        if (r.steady)
            _evidence[_current] = { r.baseFps, nowSec };

        _start = c;
        _real = _generated = _budgeted = 0;
        for (auto& n : _byGenerated)
            n = 0;
        _delivery.Reset();
        return r;
    }

    double EvidenceFps(uint32_t generated) const { return generated <= kMaxGenerated ? _evidence[generated].fps : 0.0; }

  private:
    struct Evidence
    {
        double fps = 0.0;
        double atSec = 0.0;
    };

    RefreshBudget _budget;
    CadenceEstimator _cadence;
    DeliveryWindow _delivery;
    CadenceCounts _start;
    Evidence _evidence[kMaxGenerated + 1] {};

    bool _refreshSampled = false;
    double _refreshSampledSec = 0.0;
    uint32_t _refreshHz = 0;
    double _limitFps = 0.0; // as of the last summary

    bool _hasBase = false;
    uint64_t _nextBase = 0;
    uint64_t _real = 0;
    uint64_t _generated = 0;
    uint64_t _budgeted = 0;
    uint64_t _byGenerated[kMaxGenerated + 1] {};
    uint32_t _current = kMinGenerated;
    bool _vsync = false;
};

} // namespace LsfgMfg::Adaptive
