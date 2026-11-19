#pragma once

// CPU side of the D3D12 Present route, separate from its GPU samples (DlssNr_GpuTiming.h, Region). Steady-clock
// durations of what the route already does: recording the list, submitting it, and the three existing waits
// (allocator reuse, the synthesized-motion rebuild's idle wait, PresentSync). Nothing here waits, signals or polls a
// fence. Recording contains the synth idle wait; PresentSync happens after the submit. They are overlapping subsets
// of the route's CPU time, not parts to add, and never added to a GPU interval.
//
// Gated with GpuTiming (Config::DlssNrGpuTimingRequested): with it off the route reads no clock for this. Header-only,
// stdlib only; docs/dlssnr/design/gpu-timing.md, tested by tests/nr-gpu-timing (present_cpu.cpp).

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace DlssNr::PresentCpu
{
using Clock = std::chrono::steady_clock;

enum class Wait : uint8_t
{
    Allocator,   // GetPresentCommandList: the slot's previous list still running (bounded 5 s)
    SynthIdle,   // synthesized motion rebuild: every submitted present list finished (bounded 2 s)
    PresentSync, // [DlssNr] PresentSync: this list finished before returning to the game (bounded 1 s)
    Count
};

inline const char* WaitLabel(Wait wait)
{
    switch (wait)
    {
    case Wait::Allocator:
        return "allocator_wait";
    case Wait::SynthIdle:
        return "synth_idle_wait";
    case Wait::PresentSync:
        return "present_sync_wait";
    default:
        return "unknown";
    }
}

struct WaitSample
{
    bool attempted = false; // false: no wait was needed this present, ms stays 0
    bool ok = true;         // the wait returned in time (false: timed out or the call failed)
    double ms = 0.0;
};

// One submitted present. Dropped lists are not samples.
struct Sample
{
    double recordMs = 0.0; // list acquired -> just before SubmitPresentList (contains SynthIdle)
    double submitMs = 0.0; // the SubmitPresentList call (Close + ExecuteCommandLists + Signal)
    std::array<WaitSample, static_cast<size_t>(Wait::Count)> waits {};

    WaitSample& operator[](Wait wait) { return waits[static_cast<size_t>(wait)]; }
    const WaitSample& operator[](Wait wait) const { return waits[static_cast<size_t>(wait)]; }
};

struct Stat
{
    uint64_t count = 0;
    double sumMs = 0.0, maxMs = 0.0;

    void Add(double ms)
    {
        ++count;
        sumMs += ms;
        maxMs = std::max(maxMs, ms);
    }
    double MeanMs() const { return count ? sumMs / static_cast<double>(count) : 0.0; }
};

struct WaitStat
{
    Stat waited;           // attempted waits only
    uint64_t failures = 0; // attempted and not ok: a timeout or an API error, which the wait's bool does not separate
};

struct Window
{
    uint64_t presents = 0;
    double seconds = 0.0;
    Stat record, submit;
    std::array<WaitStat, static_cast<size_t>(Wait::Count)> waits {};

    const WaitStat& operator[](Wait wait) const { return waits[static_cast<size_t>(wait)]; }
};

// The INFO line for a closed window. Mean/max over the window's presents; waits over the presents that waited.
// std::to_chars keeps dot decimals when the host process sets a comma LC_NUMERIC.
inline std::string Format(const Window& window, uint64_t run)
{
    auto uintText = [](uint64_t value)
    {
        char buf[32];
        const auto [end, error] = std::to_chars(buf, buf + sizeof(buf), value);
        return error == std::errc() ? std::string(buf, end) : std::string("0");
    };
    auto fixedText = [](double value, int precision)
    {
        char buf[64];
        const auto [end, error] = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed, precision);
        return error == std::errc() ? std::string(buf, end) : std::string("0");
    };

    std::string text =
        "DLSS-NR present cpu window: run=" + uintText(run) + " presents=" + uintText(window.presents) +
        " seconds=" + fixedText(window.seconds, 1) + " record_ms mean=" + fixedText(window.record.MeanMs(), 3) +
        " max=" + fixedText(window.record.maxMs, 3) + " submit_ms mean=" + fixedText(window.submit.MeanMs(), 3) +
        " max=" + fixedText(window.submit.maxMs, 3);
    for (size_t i = 0; i < window.waits.size(); ++i)
    {
        const auto& w = window.waits[i];
        text += ' ';
        text += WaitLabel(static_cast<Wait>(i));
        text += " attempted=" + uintText(w.waited.count) + " mean_ms=" + fixedText(w.waited.MeanMs(), 3) +
                " max_ms=" + fixedText(w.waited.maxMs, 3) + " failures=" + uintText(w.failures);
    }
    text += " clock=steady unit=submitted-presents overlapping=record-contains-synth_idle_wait;"
            "present_sync_wait-after-submit not-gpu";
    return text;
}

// Windows of `period`; Add returns true with the closed window when one ends. Thread-safe: present entries may run
// on different threads. Takes no other lock and calls nothing.
class Accumulator
{
    std::mutex mutex;
    Window current;
    Clock::time_point start {};
    bool started = false;

  public:
    bool Add(const Sample& sample, Clock::time_point now, Window* closed,
             Clock::duration period = std::chrono::seconds(5))
    {
        std::lock_guard lock(mutex);
        if (!started)
        {
            started = true;
            start = now;
        }
        ++current.presents;
        current.record.Add(sample.recordMs);
        current.submit.Add(sample.submitMs);
        for (size_t i = 0; i < current.waits.size(); ++i)
            if (sample.waits[i].attempted)
            {
                current.waits[i].waited.Add(sample.waits[i].ms);
                current.waits[i].failures += sample.waits[i].ok ? 0 : 1;
            }
        if (now - start < period)
            return false;
        current.seconds = std::chrono::duration<double>(now - start).count();
        if (closed)
            *closed = current;
        current = {};
        start = now;
        return true;
    }
};

// Clock reads only when timed: the route calls these instead of steady_clock directly.
inline Clock::time_point Mark(bool timed) { return timed ? Clock::now() : Clock::time_point {}; }
inline double SinceMs(bool timed, Clock::time_point since)
{
    return timed ? std::chrono::duration<double, std::milli>(Clock::now() - since).count() : 0.0;
}

// Wraps an existing bounded wait call; without a sample it is the plain call.
template <class Call> bool Timed(WaitSample* sample, Call&& call)
{
    if (!sample)
        return call();
    const auto begin = Clock::now();
    const bool ok = call();
    sample->attempted = true;
    sample->ok = ok;
    sample->ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    return ok;
}
} // namespace DlssNr::PresentCpu
