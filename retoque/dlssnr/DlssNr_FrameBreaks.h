#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Frame breaks (docs/dlssnr/design/frame-breaks.md): two things the pass could not say about itself.
//
// - SkipTracker: how often a frame the pass was asked to run on was shown as the game drew it between frames it did
//   run on. A refused recording (chain admission: "more than three earlier recordings on the GPU", a second queue, a
//   list that is not yet submitted) or any other skip leaves the game's own frame in place, so a pass that runs on
//   every other frame alternates the model's picture with the plain one, which is a flicker of its own. Counted always;
//   with [DlssNr] SkipStandDown on, an alternation holds the pass off for kHoldMs, so the picture is one thing for a
//   second instead of two things every frame.
// - ResetRate: how often the model was sent a history reset, by cause. A reset on a real cut is the contract; one every
//   second on a still camera is a bug somewhere, and there was no number for it.
//
// Pure: flags and times in, decisions and counts out, so tests/nr-frame-breaks pins both without a device.
namespace DlssNr::FrameBreaks
{
// The last kWindow outcomes (about half a second at 60 fps), 1 = the pass ran, 0 = the frame was left as the game
// drew it. A "break" is a run of skipped frames between two frames the pass ran on.
constexpr unsigned kWindow = 32;
static_assert(kWindow == 32, "the outcomes are the bits of one uint32_t");
constexpr unsigned kMinBreaks = 3;
constexpr int64_t kHoldMs = 1000;

// The breaks in the last `count` outcomes of `bits` (bit 0 the newest): ran, then one or more skipped, then ran.
constexpr unsigned Breaks(uint32_t bits, unsigned count)
{
    unsigned breaks = 0;
    count = count > kWindow ? kWindow : count;
    bool inGap = false;
    bool seenRan = false;
    // Oldest first.
    for (unsigned i = count; i-- > 0;)
    {
        const bool ran = ((bits >> i) & 1u) != 0;
        if (ran)
        {
            if (seenRan && inGap)
                ++breaks;
            seenRan = true;
            inGap = false;
        }
        else if (seenRan)
        {
            inGap = true;
        }
    }
    return breaks;
}

class SkipTracker
{
    uint32_t _bits = 0;
    unsigned _count = 0;
    int64_t _holdUntilMs = 0;
    uint64_t _ran = 0, _skipped = 0, _standDowns = 0, _held = 0;

    void Clear()
    {
        _bits = 0;
        _count = 0;
    }

  public:
    // Before the pass does anything: true while an alternation is being held off. Off clears the hold.
    bool StandDown(bool enabled, int64_t nowMs)
    {
        if (!enabled)
        {
            _holdUntilMs = 0;
            return false;
        }
        if (nowMs < _holdUntilMs)
        {
            ++_held;
            return true;
        }
        return false;
    }

    // What one evaluation did. Returns true on the outcome that starts a stand-down (always false when not enabled),
    // so the caller can say so once. The outcomes of a stand-down are not noted: the pass was not asked.
    bool Note(bool ran, bool enabled, int64_t nowMs)
    {
        ran ? ++_ran : ++_skipped;
        _bits = (_bits << 1) | (ran ? 1u : 0u);
        if (_count < kWindow)
            ++_count;
        if (!enabled || Breaks(_bits, _count) < kMinBreaks)
            return false;
        _holdUntilMs = nowMs + kHoldMs;
        ++_standDowns;
        Clear();
        return true;
    }

    // The alternation now in the window, enabled or not: what the log says when the option is off.
    unsigned Alternation() const { return Breaks(_bits, _count); }
    void ClearWindow() { Clear(); }

    uint64_t Ran() const { return _ran; }
    uint64_t Skipped() const { return _skipped; }
    uint64_t StandDowns() const { return _standDowns; }
    uint64_t HeldFrames() const { return _held; }
    void Reset() { *this = SkipTracker {}; }
};

enum class ResetCause : uint8_t
{
    Game,     // the game's own flag on its evaluate
    Policy,   // ZeroGuideReset or the dead-motion verdict, for the model's sake
    SceneCut, // the synthesized motion's scene cut
    Internal, // anything this fork changed: a setting, a size, a route, a failed pass
    Count
};

struct ResetSummary
{
    uint32_t total = 0;
    std::array<uint32_t, static_cast<size_t>(ResetCause::Count)> byCause {};
    uint32_t peakPerSecond = 0;
};

// Resets sent to the model, kept by time so a rate can be read back. Capacity is a bound, not a limit on counting: a
// full ring drops the oldest, and a rate above kCapacity in the window reads as kCapacity.
class ResetRate
{
  public:
    static constexpr size_t kCapacity = 128;
    static constexpr int64_t kReportMs = 10000;

  private:
    struct Entry
    {
        int64_t ms = 0;
        ResetCause cause = ResetCause::Internal;
    };
    std::array<Entry, kCapacity> _ring {};
    size_t _next = 0;
    size_t _size = 0;
    uint64_t _total = 0;
    int64_t _reportedAtMs = 0;
    bool _reported = false;

  public:
    void Note(ResetCause cause, int64_t nowMs)
    {
        _ring[_next] = { nowMs, cause };
        _next = (_next + 1) % kCapacity;
        if (_size < kCapacity)
            ++_size;
        ++_total;
    }

    // The resets in the last windowMs, by cause, and the most in any one second of it.
    ResetSummary Summarize(int64_t nowMs, int64_t windowMs) const
    {
        ResetSummary s;
        std::array<int64_t, kCapacity> times {};
        size_t n = 0;
        for (size_t i = 0; i < _size; ++i)
        {
            const Entry& e = _ring[i];
            if (nowMs - e.ms > windowMs || e.ms > nowMs)
                continue;
            ++s.total;
            ++s.byCause[static_cast<size_t>(e.cause)];
            times[n++] = e.ms;
        }
        // Peak per second: the most times within 1000 ms of each other. The ring is not sorted by time once wrapped.
        for (size_t a = 0; a < n; ++a)
        {
            uint32_t within = 0;
            for (size_t b = 0; b < n; ++b)
                if (times[b] >= times[a] && times[b] - times[a] < 1000)
                    ++within;
            if (within > s.peakPerSecond)
                s.peakPerSecond = within;
        }
        return s;
    }

    // Whether to say the rate now: at most every kReportMs, and only when something was sent in that time.
    bool ShouldReport(int64_t nowMs)
    {
        if (_reported && nowMs - _reportedAtMs < kReportMs)
            return false;
        if (Summarize(nowMs, kReportMs).total == 0)
            return false;
        _reported = true;
        _reportedAtMs = nowMs;
        return true;
    }

    uint64_t Total() const { return _total; }
    void Reset() { *this = ResetRate {}; }
};
} // namespace DlssNr::FrameBreaks
