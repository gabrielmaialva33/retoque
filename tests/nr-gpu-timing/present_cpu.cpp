// The Present route's CPU stream (retoque/dlssnr/DlssNr_PresentCpuTiming.h): windows, waits, formatting, the
// untimed path. Known durations go in; they must come out only in CPU fields, never summed.
#include "retoque/dlssnr/DlssNr_PresentCpuTiming.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace DlssNr::PresentCpu;

int main()
{
    unsigned cases = 0;
    const auto t0 = Clock::time_point {} + std::chrono::hours(1);

    {
        Accumulator accumulator;
        Window closed;
        Sample a;
        a.recordMs = 0.4;
        a.submitMs = 0.1;
        assert(!accumulator.Add(a, t0, &closed));
        Sample b;
        b.recordMs = 2.4;
        b.submitMs = 0.3;
        b[Wait::SynthIdle] = { true, true, 2.0 };
        b[Wait::PresentSync] = { true, false, 1000.0 };
        assert(!accumulator.Add(b, t0 + std::chrono::seconds(2), &closed));
        Sample c;
        c.recordMs = 0.2;
        c.submitMs = 0.2;
        c[Wait::Allocator] = { true, true, 0.5 };
        assert(accumulator.Add(c, t0 + std::chrono::seconds(5), &closed));
        assert(closed.presents == 3 && closed.seconds > 4.99 && closed.seconds < 5.01);
        assert(closed.record.count == 3 && closed.record.maxMs == 2.4);
        assert(closed.record.MeanMs() > 0.999 && closed.record.MeanMs() < 1.001);
        assert(closed.submit.maxMs == 0.3);
        assert(closed[Wait::Allocator].waited.count == 1 && closed[Wait::Allocator].waited.maxMs == 0.5);
        assert(closed[Wait::SynthIdle].waited.count == 1 && closed[Wait::SynthIdle].failures == 0);
        assert(closed[Wait::PresentSync].waited.count == 1 && closed[Wait::PresentSync].failures == 1);
        ++cases; // Waits count only presents that waited; a failure (timeout or error) is counted, its duration kept.

        Sample d;
        d.recordMs = 0.7;
        assert(!accumulator.Add(d, t0 + std::chrono::seconds(6), &closed));
        assert(accumulator.Add(d, t0 + std::chrono::seconds(10), &closed) && closed.presents == 2 &&
               closed[Wait::PresentSync].waited.count == 0);
        ++cases; // A new window starts empty.

        const auto text = Format(closed, 7);
        assert(text.find("DLSS-NR present cpu window: run=7 presents=2 ") == 0);
        assert(text.find("record_ms mean=0.700 max=0.700") != std::string::npos);
        assert(text.find("present_sync_wait attempted=0 mean_ms=0.000") != std::string::npos);
        assert(text.find("failures=") != std::string::npos && text.find("timeouts=") == std::string::npos);
        assert(text.find("clock=steady") != std::string::npos && text.find("not-gpu") != std::string::npos);
        ++cases; // One self-describing line; no total that adds overlapping parts.
    }

    {
        int calls = 0;
        assert(Timed(nullptr,
                     [&]
                     {
                         ++calls;
                         return true;
                     }));
        WaitSample sample;
        assert(!Timed(&sample,
                      [&]
                      {
                          ++calls;
                          return false;
                      }));
        assert(calls == 2 && sample.attempted && !sample.ok && sample.ms >= 0.0);
        assert(Mark(false) == Clock::time_point {} && SinceMs(false, Clock::now()) == 0.0);
        ++cases; // Untimed: the wait still runs exactly once, and no clock is read for it.
    }

    if (cases != 4)
        return 2;
    std::cout << "PASS " << cases << " Present route CPU window cases\n";
    return 0;
}
