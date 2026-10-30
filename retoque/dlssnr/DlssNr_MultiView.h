#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

// Several views drawn into one colour allocation within one frame: a triple-screen rig (one projection per monitor,
// side by side, often plus one combined picture), or split screen. The game calls the upscaler once per view, each
// with its own active rectangle. NR keeps one model and one history, so those calls hand it a different picture on
// every evaluate: the extents alternate and the settling gates hold the model off, or, where the views share a size,
// one history is fed every screen in turn; a view off the origin is declined before the upscaler and served over the
// whole output after it. Assetto Corsa EVO's multi-monitor logs showed the model rebuilt on every call, never
// evaluated.
//
// So NR stands down while it happens: every evaluate of such a frame, counted, with the reason logged. There is no
// setting and no per-view model; one model cannot serve several pictures, and running one per view is a separate
// design that has not been measured.
//
// The detector is janblade's (OptiScaler-F5-DLSSNR-Multipass, commit 1105908f, DlssNr_Views.h; GPL-3.0), adapted:
// only the detection and the default stand-down are taken. Its view pick (a setting choosing one view) and the per-view
// models that followed (358bc0cb) are not. docs/dlssnr/design/dx12-robustness.md, tested by tests/nr-multi-view.
//
// Nothing here touches D3D12: the caller hands in what it read from the parameter block.
namespace DlssNr::MultiView
{
struct Rect
{
    uint32_t x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Rect&) const = default;
};

// What one evaluate reads: the colour allocation, and the active rectangle inside it.
struct Seen
{
    uint64_t allocWidth = 0;
    uint32_t allocHeight = 0;
    uint32_t allocFormat = 0;
    Rect rect;

    bool SameAllocation(const Seen& o) const
    {
        return allocWidth == o.allocWidth && allocHeight == o.allocHeight && allocFormat == o.allocFormat;
    }
    bool SameView(const Seen& o) const { return SameAllocation(o) && rect == o.rect; }
};

enum class Verdict
{
    Run,       // one view per frame (or no observed frame to judge by)
    StandDown, // several views in one allocation: NR does not run on this evaluate
};

// Distinct views remembered per frame. A ninth is not remembered; two are enough to decide.
constexpr size_t kMaxViews = 8;
// Consecutive frames with several views before the layout counts as found. One frame is not enough: a present
// counter that stopped advancing would put a game's ordinary resolution change into "one frame".
constexpr uint32_t kLatchEpochs = 2;
// Judged frames with one view each before the layout counts as gone: a menu or a loading screen between races is not
// enough to forget it.
constexpr uint32_t kQuietEpochs = 300;

struct Result
{
    Verdict verdict = Verdict::Run;
    bool latchedNow = false;   // this evaluate found the layout
    bool unlatchedNow = false; // ... or the frame before it was the kQuietEpochs-th without one
    uint32_t viewsInEpoch = 0; // distinct views seen in this frame so far, this one included
    uint64_t standDowns = 0;   // evaluates stood down this session, this one included
};

class Detector
{
  public:
    // epoch is the observed present count. Zero means no present has been observed: nothing is judged, because every
    // evaluate would fall in one "frame" and a game's ordinary resolution changes would look like several views.
    // Each NR seam of one evaluate may note it; the same view twice in one frame counts once.
    Result Note(uint64_t epoch, const Seen& seen)
    {
        std::lock_guard<std::mutex> lock(_lock);
        Result result;

        if (epoch == 0)
        {
            result.standDowns = _standDowns;
            return result;
        }

        if (!_haveEpoch || epoch != _epoch)
        {
            if (_haveEpoch)
                result.unlatchedNow = EndEpoch();
            _epoch = epoch;
            _haveEpoch = true;
            _epochCount = 0;
            _epochMulti = false;
        }

        bool known = false;
        for (size_t i = 0; i < _epochCount; ++i)
            known = known || _epochViews[i].SameView(seen);
        if (!known && _epochCount < kMaxViews)
            _epochViews[_epochCount++] = seen;
        result.viewsInEpoch = static_cast<uint32_t>(_epochCount);

        // Several views in this evaluate's allocation: that is the layout. Two views in two different textures are a
        // different thing (a mirror, a scope, a portrait) and are left to the gates that already handle them.
        size_t sameAllocation = 0;
        for (size_t i = 0; i < _epochCount; ++i)
            sameAllocation += _epochViews[i].SameAllocation(seen) ? 1 : 0;

        if (sameAllocation >= 2 && !_epochMulti)
        {
            _epochMulti = true;
            _quiet = 0;
            if (_multiEpochs < kLatchEpochs)
                ++_multiEpochs;
            if (!_latched && _multiEpochs >= kLatchEpochs)
            {
                _latched = true;
                result.latchedNow = true;
            }
        }

        if (_latched)
        {
            result.verdict = Verdict::StandDown;
            ++_standDowns;
        }

        result.standDowns = _standDowns;
        return result;
    }

    bool Active() const
    {
        std::lock_guard<std::mutex> lock(_lock);
        return _latched;
    }

    uint64_t StandDowns() const
    {
        std::lock_guard<std::mutex> lock(_lock);
        return _standDowns;
    }

    // Host fixtures only; production keeps one detector for the process.
    void Reset()
    {
        std::lock_guard<std::mutex> lock(_lock);
        _haveEpoch = false;
        _epoch = 0;
        _epochCount = 0;
        _epochMulti = false;
        _multiEpochs = 0;
        _latched = false;
        _quiet = 0;
        _standDowns = 0;
    }

  private:
    // True when the layout is gone after the frame that just ended.
    bool EndEpoch()
    {
        if (_epochMulti)
            return false;
        _multiEpochs = 0;
        if (!_latched)
            return false;
        if (++_quiet < kQuietEpochs)
            return false;
        _latched = false;
        _quiet = 0;
        return true;
    }

    mutable std::mutex _lock;
    bool _haveEpoch = false;
    uint64_t _epoch = 0;
    Seen _epochViews[kMaxViews];
    size_t _epochCount = 0;
    bool _epochMulti = false;
    uint32_t _multiEpochs = 0;
    bool _latched = false;
    uint32_t _quiet = 0;
    uint64_t _standDowns = 0;
};
} // namespace DlssNr::MultiView
