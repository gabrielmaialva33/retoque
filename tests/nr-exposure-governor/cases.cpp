#include <dlssnr/DlssNr_Exposure.h>
#include <dlssnr/DlssNr_ExposureGovernor.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace DlssNr::ExposureGovernor;

namespace
{
float Stops(float a, float b) { return std::log2(b / a); }

bool Near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

// Frames at a rate, a target that does not move: how many stops the applied value has travelled.
float Run(State& state, float target, float seconds, int frames, bool unreadable = false)
{
    float applied = state.Applied();
    for (int i = 0; i < frames; ++i)
        applied = state.Update(target, seconds, unreadable, false).applied;
    return applied;
}
} // namespace

int main()
{
    // The hold's range is the plausibility latch's, so the two cannot drift apart.
    static_assert(kPlausibleMinImpliedWhite == DlssNr::Exposure::PlausibilityLatch::kMinImpliedWhite);
    static_assert(kPlausibleMaxImpliedWhite == DlssNr::Exposure::PlausibilityLatch::kMaxImpliedWhite);
    assert(PlausibleImpliedWhite(1.0f) && PlausibleImpliedWhite(0.02f) && PlausibleImpliedWhite(512.0f));
    assert(!PlausibleImpliedWhite(0.0199f) && !PlausibleImpliedWhite(512.1f));
    assert(!PlausibleImpliedWhite(NAN) && !PlausibleImpliedWhite(INFINITY));

    // 1. The first reading is taken as it is; nothing is carried from before.
    {
        State s;
        assert(!s.Primed());
        const auto r = s.Update(8.0f, 0.016f, false, false);
        assert(r.outcome == Outcome::Primed && r.applied == 8.0f && s.Primed());
    }

    // 2. A target that does not move leaves the applied value exactly where it was (inside the hysteresis band).
    {
        State s;
        s.Update(8.0f, 0.016f, false, false);
        for (float target : { 8.0f, 8.0f * std::exp2(0.10f), 8.0f * std::exp2(-0.10f), 8.0f * std::exp2(0.149f) })
        {
            const auto r = s.Update(target, 0.016f, false, false);
            assert(r.outcome == Outcome::Held && r.applied == 8.0f && !s.Tracking());
        }
    }

    // 3. Rate limits per second, whatever the frame rate: up 4 stops/s, down 0.5 stops/s.
    for (float dt : { 1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f })
    {
        const int frames = (int) std::lround(1.0f / dt);
        State up;
        up.Update(1.0f, dt, false, false);
        const float raised = Run(up, 1024.0f, dt, frames);
        assert(Near(Stops(1.0f, raised), 4.0f, 0.05f));

        State down;
        down.Update(1024.0f, dt, false, false);
        const float lowered = Run(down, 1.0f, dt, frames);
        assert(Near(Stops(lowered, 1024.0f), 0.5f, 0.02f));
    }

    // 4. The whole way when the limit does not bite, and it stops at the target, not past it.
    {
        State s;
        s.Update(1.0f, 0.016f, false, false);
        const auto r = s.Update(std::exp2(0.5f), 0.5f, false, false);
        assert(r.outcome == Outcome::Followed && Near(Stops(1.0f, r.applied), 0.5f));
        const auto again = s.Update(std::exp2(0.5f), 0.5f, false, false);
        assert(again.outcome == Outcome::Held);
    }

    // 5. Hysteresis: leaves the band at 0.15 stops, then tracks until within 0.02.
    {
        State s;
        s.Update(1.0f, 0.016f, false, false);
        const float target = std::exp2(0.16f);
        auto r = s.Update(target, 1.0f, false, false);
        assert(s.Tracking() && r.outcome == Outcome::Followed);
        // Now 0.0 stops away: settled. Move the target 0.05 away: inside engage, outside settle: still tracking.
        s.Update(target, 0.016f, false, false);
        assert(!s.Tracking());
    }
    {
        State s;
        s.Update(1.0f, 0.016f, false, false);
        s.Update(std::exp2(0.2f), 0.001f, false, false); // tracking, barely moved
        assert(s.Tracking());
        const float applied = s.Applied();
        // 0.05 stops away: inside the engage band but outside the settle band, so it keeps tracking.
        const auto r = s.Update(applied * std::exp2(0.05f), 1.0f, false, false);
        assert(r.outcome == Outcome::Followed && Near(Stops(applied, r.applied), 0.05f));
    }

    // 6. An unreadable frame (black frame, loading screen) holds, keeps the hysteresis state and never moves it.
    {
        State s;
        s.Update(16.0f, 0.016f, false, false);
        const auto held = s.Update(0.01f, 1.0f, true, false);
        assert(held.outcome == Outcome::Held && held.applied == 16.0f);
        assert(Run(s, 1000.0f, 0.016f, 600, true) == 16.0f);
        // And the scene after it is followed at the limit, not snapped.
        const auto next = s.Update(32.0f, 0.016f, false, false);
        assert(next.outcome == Outcome::Limited && Near(Stops(16.0f, next.applied), kAttackStopsPerSecond * 0.016f));
    }

    // 7. A start on an unreadable frame takes the first readable one as it is, and passes the target meanwhile.
    {
        State s;
        const auto first = s.Update(0.001f, 0.016f, true, false);
        assert(first.outcome == Outcome::Passed && first.applied == 0.001f && !s.Primed() && s.Pending());
        const auto second = s.Update(5.0f, 0.016f, false, false);
        assert(second.outcome == Outcome::Primed && second.applied == 5.0f && !s.Pending());
    }

    // 8. The game's reset re-primes at once; a reset on an unreadable frame waits for the first readable one.
    {
        State s;
        s.Update(1.0f, 0.016f, false, false);
        const auto r = s.Update(64.0f, 0.016f, false, true);
        assert(r.outcome == Outcome::Primed && r.applied == 64.0f);

        s.Update(64.0f, 0.016f, false, false);
        const auto held = s.Update(0.5f, 0.016f, true, true);
        assert(held.outcome == Outcome::Held && held.applied == 64.0f && s.Pending());
        const auto after = s.Update(4.0f, 0.016f, false, false);
        assert(after.outcome == Outcome::Primed && after.applied == 4.0f);
    }

    // 9. A pause longer than the gap says the held value describes another scene: re-primed, not slewed.
    {
        State s;
        s.Update(1.0f, 0.016f, false, false);
        assert(s.Update(100.0f, kGapSeconds + 0.1f, false, false).outcome == Outcome::Primed);
        State t;
        t.Update(1.0f, 0.016f, false, false);
        // A long frame inside the gap is limited by kMaxStepSeconds, not by the time it took.
        const auto r = t.Update(1e6f, kGapSeconds - 0.1f, false, false);
        assert(r.outcome == Outcome::Limited && Near(Stops(1.0f, r.applied), kAttackStopsPerSecond * kMaxStepSeconds));
    }

    // 10. Two evaluates of one frame (no time between them) do not step it twice.
    {
        State s;
        s.Update(1.0f, 0.016f, false, false);
        const auto r = s.Update(1024.0f, 0.0f, false, false);
        assert(r.applied == 1.0f);
    }

    // 11. Nothing to govern passes untouched and does not disturb the state.
    {
        State s;
        s.Update(2.0f, 0.016f, false, false);
        for (float target : { NAN, INFINITY, -1.0f, 0.0f })
        {
            const auto r = s.Update(target, 0.016f, false, true);
            assert(r.outcome == Outcome::Passed);
        }
        assert(s.Applied() == 2.0f && !s.Pending());
        s.Reset();
        assert(!s.Primed() && !s.Tracking() && !s.Pending());
    }

    // 12. Negative or NaN time is no time.
    {
        State s;
        s.Update(1.0f, 0.016f, false, false);
        assert(s.Update(100.0f, -1.0f, false, false).applied == 1.0f);
        assert(s.Update(100.0f, NAN, false, false).applied == 1.0f);
    }

    // 13. A fade: the game's exposure walks 16 stops over two seconds; the applied value follows the up direction
    // within one frame's step and the down direction at no more than 0.5 stops a second, and is monotonic.
    {
        State s;
        s.Update(1.0f, 1.0f / 60.0f, false, false);
        float previous = 1.0f;
        for (int i = 0; i < 120; ++i)
        {
            const float target = std::exp2(16.0f * (float) i / 120.0f);
            const float applied = s.Update(target, 1.0f / 60.0f, false, false).applied;
            assert(applied >= previous && applied <= target * 1.0001f);
            previous = applied;
        }
        State d;
        d.Update(65536.0f, 1.0f / 60.0f, false, false);
        previous = 65536.0f;
        for (int i = 0; i < 120; ++i)
        {
            const float target = 65536.0f * std::exp2(-16.0f * (float) i / 120.0f);
            const float applied = d.Update(target, 1.0f / 60.0f, false, false).applied;
            assert(applied <= previous && Stops(applied, previous) <= kReleaseStopsPerSecond / 60.0f + 1e-4f);
            previous = applied;
        }
        assert(Stops(previous, 65536.0f) <= 1.01f); // 2 s at 0.5 stops/s
    }

    std::puts("exposure governor: ok");
    return 0;
}
