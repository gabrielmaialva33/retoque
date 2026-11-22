// Frame breaks (docs/dlssnr/design/frame-breaks.md): the production skip tracker and reset-rate counter.
#include <dlssnr/DlssNr_FrameBreaks.h>

#include <cassert>
#include <cstdio>

using namespace DlssNr::FrameBreaks;

namespace
{
// Feed a pattern of outcomes ('R' ran, 's' skipped), one per 16 ms; returns the time after the last.
int64_t Feed(SkipTracker& t, const char* pattern, bool enabled, int64_t ms, bool* started = nullptr)
{
    for (const char* p = pattern; *p; ++p, ms += 16)
        if (t.Note(*p == 'R', enabled, ms) && started)
            *started = true;
    return ms;
}

uint32_t Bits(const char* pattern, unsigned& count)
{
    uint32_t bits = 0;
    count = 0;
    for (const char* p = pattern; *p; ++p, ++count)
        bits = (bits << 1) | (*p == 'R' ? 1u : 0u);
    return bits;
}

unsigned BreaksOf(const char* pattern)
{
    unsigned count;
    const uint32_t bits = Bits(pattern, count);
    return Breaks(bits, count);
}
} // namespace

int main()
{
    // ---- What a break is: skipped frames between two frames the pass ran on.
    assert(BreaksOf("") == 0 && BreaksOf("RRRR") == 0 && BreaksOf("ssss") == 0);
    assert(BreaksOf("ssssRRRR") == 0);   // settling at the start is not a break
    assert(BreaksOf("RRRRssss") == 0);   // nor is a stop
    assert(BreaksOf("RsR") == 1);
    assert(BreaksOf("RssssR") == 1);     // a long gap is one break
    assert(BreaksOf("RsRsRsR") == 3);    // every other frame
    assert(BreaksOf("sRsRsR") == 2);
    assert(BreaksOf("RRsRRsRR") == 2);

    // ---- Not enabled: counted, never held.
    {
        SkipTracker t;
        bool started = false;
        const int64_t ms = Feed(t, "RsRsRsRsRsRsRsRs", false, 0, &started);
        assert(!started && !t.StandDown(false, ms) && t.StandDowns() == 0);
        assert(t.Alternation() >= kMinBreaks);
        assert(t.Ran() == 8 && t.Skipped() == 8);
    }

    // ---- Enabled: three breaks start a hold of kHoldMs, then the pass is asked again and the window is clean.
    {
        SkipTracker t;
        bool started = false;
        int64_t ms = Feed(t, "RsRsRs", true, 1000, &started);
        assert(!started);                       // two breaks so far
        started = false;
        ms = Feed(t, "R", true, ms, &started);  // the third
        assert(started && t.StandDowns() == 1);
        const int64_t at = ms - 16;
        assert(t.StandDown(true, at + 1));
        assert(t.StandDown(true, at + kHoldMs - 1));
        assert(!t.StandDown(true, at + kHoldMs));  // asked again
        assert(t.HeldFrames() == 2);
        assert(t.Alternation() == 0);              // and the window starts over
        // Off in between clears the hold.
        SkipTracker u;
        Feed(u, "RsRsRsR", true, 0);
        assert(u.StandDown(true, 100));
        assert(!u.StandDown(false, 101) && !u.StandDown(true, 102));
    }

    // ---- A healthy run never holds, whatever its length; a rare skip does not.
    {
        SkipTracker t;
        bool started = false;
        int64_t ms = 0;
        for (int i = 0; i < 20; ++i)
            ms = Feed(t, "RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRs", true, ms, &started);  // one skip in 41, window 32
        assert(!started && t.StandDowns() == 0);
    }

    // ---- The window forgets: breaks older than kWindow outcomes do not count.
    {
        SkipTracker t;
        bool started = false;
        int64_t ms = Feed(t, "RsRsR", true, 0, &started);       // two breaks
        ms = Feed(t, "RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR", true, ms, &started);  // 32 clean outcomes push them out
        ms = Feed(t, "RsR", true, ms, &started);                // one break
        assert(!started && t.Alternation() == 1);
    }

    // ---- A long clean run, then one break: exactly one, however many outcomes have passed.
    {
        SkipTracker t;
        bool started = false;
        int64_t ms = 0;
        for (int i = 0; i < 20; ++i)
            ms = Feed(t, "RRRRRRRRRR", true, ms, &started);
        ms = Feed(t, "RsRRRRR", true, ms, &started);
        assert(!started && t.Alternation() == 1);
        ms = Feed(t, "sRRRR", true, ms, &started);
        assert(!started && t.Alternation() == 2);
    }

    // ---- Counts are totals, resettable.
    {
        SkipTracker t;
        Feed(t, "RsR", false, 0);
        assert(t.Ran() == 2 && t.Skipped() == 1);
        t.Reset();
        assert(t.Ran() == 0 && t.Skipped() == 0 && t.Alternation() == 0);
    }

    // ---- Reset rate: counts by cause in a window, the peak per second, the report cadence.
    {
        ResetRate r;
        assert(!r.ShouldReport(0));                                   // nothing sent: nothing to say
        r.Note(ResetCause::Game, 1000);
        r.Note(ResetCause::Internal, 1100);
        r.Note(ResetCause::Internal, 1200);
        r.Note(ResetCause::Policy, 1300);
        r.Note(ResetCause::SceneCut, 5000);
        auto s = r.Summarize(5100, ResetRate::kReportMs);
        assert(s.total == 5 && s.byCause[0] == 1 && s.byCause[1] == 1 && s.byCause[2] == 1 && s.byCause[3] == 2);
        assert(s.peakPerSecond == 4);
        s = r.Summarize(5100, 1000);                                  // only the last second
        assert(s.total == 1 && s.byCause[static_cast<size_t>(ResetCause::SceneCut)] == 1);
        assert(r.ShouldReport(5100));
        assert(!r.ShouldReport(5100 + ResetRate::kReportMs - 1));     // not again within the cadence, and nothing new
        r.Note(ResetCause::Game, 5200);
        assert(!r.ShouldReport(5100 + ResetRate::kReportMs - 1));
        assert(r.ShouldReport(5100 + ResetRate::kReportMs));
        assert(r.Total() == 6);
        // Everything older than the window is not reported.
        assert(!r.ShouldReport(5100 + 3 * ResetRate::kReportMs));
        // A reset every frame at 60 fps for a second reads as ~60 a second.
        ResetRate every;
        for (int i = 0; i < 60; ++i)
            every.Note(ResetCause::Internal, 100000 + i * 16);
        assert(every.Summarize(101000, 1000).peakPerSecond >= 60);
        // The ring is bounded and keeps counting.
        ResetRate flood;
        for (int i = 0; i < 1000; ++i)
            flood.Note(ResetCause::Game, i);
        assert(flood.Total() == 1000 && flood.Summarize(999, 100000).total == ResetRate::kCapacity);
        flood.Reset();
        assert(flood.Total() == 0);
    }

    std::puts("frame breaks: ok");
    return 0;
}
