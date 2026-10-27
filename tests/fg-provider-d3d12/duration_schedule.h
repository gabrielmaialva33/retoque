#pragma once
#include <cstdint>

// Strict bounded decimal parsing also rejects signs before any unsigned conversion.
inline bool ParseDurationSeconds(const char* text, unsigned& seconds)
{
    unsigned value = 0;
    if (!text || !*text)
        return false;
    for (const char* digit = text; *digit; ++digit)
    {
        if (*digit < '0' || *digit > '9')
            return false;
        value = value * 10 + static_cast<unsigned>(*digit - '0');
        if (value > 120)
            return false;
    }
    if (value < 5)
        return false;
    seconds = value;
    return true;
}

// Observe real GetState sample timestamps; warmup ends after the 32nd sample.
// Windows contain whole call intervals and never split or interpolate a delta.
class DurationSchedule
{
    unsigned minimumFrames_;
    std::uint64_t requestedUs_;
    unsigned requiredWindows_;
    std::uint64_t steadyStartUs_ = 0, windowStartUs_ = 0, endUs_ = 0;
    unsigned completeWindows_ = 0;

  public:
    DurationSchedule(unsigned minimumFrames, unsigned seconds)
        : minimumFrames_(minimumFrames), requestedUs_(std::uint64_t(seconds) * 1000000), requiredWindows_(seconds / 5)
    {
    }

    void Observe(unsigned completed, std::uint64_t sampleUs)
    {
        endUs_ = sampleUs;
        if (completed == 32)
            steadyStartUs_ = windowStartUs_ = sampleUs;
        else if (completed > 32 && sampleUs - windowStartUs_ >= 5000000)
        {
            ++completeWindows_;
            windowStartUs_ = sampleUs;
        }
    }

    bool NeedsMore(unsigned completed) const
    {
        return completed < minimumFrames_ ||
               (requestedUs_ &&
                (completed <= 32 || endUs_ - steadyStartUs_ < requestedUs_ || completeWindows_ < requiredWindows_));
    }
    std::uint64_t RequestedUs() const { return requestedUs_; }
    std::uint64_t SteadyStartUs() const { return steadyStartUs_; }
    std::uint64_t EndUs() const { return endUs_; }
    unsigned CompleteWindows() const { return completeWindows_; }
};
