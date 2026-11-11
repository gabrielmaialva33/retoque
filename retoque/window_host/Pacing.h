// Which captured frames the host processes, and what it says about their age. Pure and Windows-free, held by
// tests/nr-window-host-rules.
//
// The host is event driven: Windows Graphics Capture signals a frame, the host takes the newest one in the pool and
// drops the rest. A frame is processed only if the compositor stamped it later than the last one processed. A gap of
// more than 100 ms, a new crop, a resize or a resume from a pause starts a new stream: said in the trace, because the
// present pass's history (and SynthMotion's previous frame) then belongs to another picture. Version 1 has no way to
// tell the pass so; ZeroGuideReset and the pass's own extent checks are what it has (nr-anything.md, "Decisions").
//
// Times are the compositor's SystemRelativeTime, in 100 ns units of the QPC clock.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace RetoqueWindowHost::Pacing
{
inline constexpr int64_t kTicksPerSecond = 10'000'000;      // 100 ns units
inline constexpr int64_t kStreamGap = kTicksPerSecond / 10; // 100 ms
inline constexpr uint64_t kCropWaitMs = 1000;

// A QueryPerformanceCounter reading in the same 100 ns units as SystemRelativeTime, without overflowing for any
// uptime (the naive qpc * 10^7 overflows int64 after about ten days at a 10 MHz counter).
inline int64_t QpcToTicks(int64_t qpc, int64_t frequency)
{
    if (qpc <= 0 || frequency <= 0)
        return 0;
    return (qpc / frequency) * kTicksPerSecond + (qpc % frequency) * kTicksPerSecond / frequency;
}

enum class Verdict
{
    Drop,          // not newer than the last processed frame, or unstamped
    Take,          // the next frame of the same stream
    TakeNewStream, // processed, and the trace says the stream restarted
};

struct FrameDecision
{
    Verdict verdict = Verdict::Drop;
    const char* reason = nullptr; // why it was dropped or why a new stream starts
};

class Pacer
{
  public:
    // geometryChanged: the crop or extent differs from the last processed frame's. resumed: the first poll after a
    // pause.
    FrameDecision Next(int64_t frameTime, bool geometryChanged, bool resumed)
    {
        if (frameTime <= 0)
            return { Verdict::Drop, "the compositor gave the frame no time" };
        if (last_ != 0 && frameTime <= last_)
            return { Verdict::Drop, "not newer than the last frame processed" };
        const int64_t previous = last_;
        last_ = frameTime;
        if (previous == 0)
            return { Verdict::TakeNewStream, "the first frame" };
        if (resumed)
            return { Verdict::TakeNewStream, "resumed after a pause" };
        if (geometryChanged)
            return { Verdict::TakeNewStream, "the source's size or crop changed" };
        if (frameTime - previous > kStreamGap)
            return { Verdict::TakeNewStream, "more than 100 ms since the last frame" };
        return { Verdict::Take, nullptr };
    }

    int64_t Last() const { return last_; }

  private:
    int64_t last_ = 0;
};

// A frame whose crop cannot be confirmed (WGC may still deliver the old raster just after a resize) is dropped, for up
// to a second; after that the session stops rather than guess.
class CropWait
{
  public:
    // True while dropping is still allowed; false once the wait has run out.
    bool Failed(uint64_t nowMs)
    {
        if (!waiting_)
        {
            waiting_ = true;
            since_ = nowMs;
        }
        return nowMs - since_ < kCropWaitMs;
    }
    void Confirmed() { waiting_ = false; }
    bool Waiting() const { return waiting_; }

  private:
    bool waiting_ = false;
    uint64_t since_ = 0;
};

// Source age: from the compositor's stamp to the host's Present, in milliseconds. p50 and p95 by nearest rank, over a
// bounded window of the most recent samples.
class AgeStats
{
  public:
    static constexpr size_t kWindow = 4096;

    void Add(double ms)
    {
        if (!(ms >= 0.0) || ms > 60'000.0) // NaN, negative or a minute: a clock fault, not an age
            return;
        if (samples_.size() == kWindow)
            samples_.erase(samples_.begin());
        samples_.push_back(ms);
    }

    size_t Count() const { return samples_.size(); }

    // p in [0, 100]; 0 with no samples.
    double Percentile(double p) const
    {
        if (samples_.empty())
            return 0.0;
        std::vector<double> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());
        const double clamped = std::clamp(p, 0.0, 100.0);
        size_t rank = size_t(std::ceil(clamped / 100.0 * double(sorted.size())));
        rank = std::clamp<size_t>(rank, 1, sorted.size());
        return sorted[rank - 1];
    }

  private:
    std::vector<double> samples_;
};
} // namespace RetoqueWindowHost::Pacing
