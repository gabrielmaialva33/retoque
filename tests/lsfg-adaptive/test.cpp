// tests/lsfg-adaptive: the log-only adaptive multiplier adviser of the MFG presenter
// (retoque/lsfg/presenter/MfgAdaptive.h), compiled unmodified with the real XeFG_DynamicCount.h under ASan/UBSan.
// docs/dlssnr/design/lsfg-adaptive-shadow.md. The MAKO vectors are MAKO's own expectations
// (engine/mako-render/tests/presentation_policy_tests.cpp and adaptive_scheduler_tests.cpp, commit a321c2a),
// re-expressed against our reimplementation; none of MAKO's code is compiled here.

#include <lsfg/presenter/MfgAdaptive.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_checks;                                                                                                    \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
        }                                                                                                              \
    } while (0)

using namespace LsfgMfg::Adaptive;

// A real frame every 1 / fps seconds from `start`; returns the sum of what the budget allowed.
static uint64_t BudgetRun(RefreshBudget& budget, double fps, uint32_t frames, uint32_t refreshHz, uint32_t maxGenerated,
                          double start = 0.0)
{
    uint64_t generated = 0;
    for (uint32_t frame = 0; frame <= frames; ++frame)
        generated += budget.Plan(start + double(frame) / fps, refreshHz, maxGenerated);
    return generated;
}

// MAKO presentation_policy_tests.cpp:1702-1725: 63 fps at x2 on 120 Hz budgets about 120 displayed fps, 100 fps
// synthesizes only the displayable remainder; the first real frame is not generated while timing warms up.
static void BudgetMakoVectors()
{
    RefreshBudget budget;
    const uint64_t at63 = BudgetRun(budget, 63.0, 630, 120, 1);
    std::printf("budget 63 fps x2 @120 Hz: %llu generated of 630\n", (unsigned long long) at63);
    CHECK(at63 >= 565 && at63 <= 575);

    budget.Reset();
    const uint64_t at100 = BudgetRun(budget, 100.0, 600, 120, 1);
    std::printf("budget 100 fps x2 @120 Hz: %llu generated of 600\n", (unsigned long long) at100);
    CHECK(at100 >= 115 && at100 <= 125);
}

static void BudgetGuards()
{
    RefreshBudget budget;

    // Refresh unknown (or nothing to generate): the maximum, as MAKO does without a confirmed refresh.
    CHECK(budget.Plan(0.0, 0, 3) == 3);
    CHECK(budget.Plan(0.016, 0, 2) == 2);
    CHECK(budget.Plan(0.032, 120, 0) == 0);

    // First timed frame: nothing generated.
    budget.Reset();
    CHECK(budget.Plan(1.0, 120, 3) == 0);
    CHECK(budget.Plan(1.0 + 1.0 / 30.0, 120, 3) == 3); // 4 outputs per real frame fit

    // A gap longer than 250 ms (loading screen, alt-tab) or a clock going backwards: nothing, and no backlog.
    CHECK(budget.Plan(1.5, 120, 3) == 0);
    CHECK(budget.Plan(1.4, 120, 3) == 0);
    CHECK(budget.Plan(1.4 + 1.0 / 30.0, 120, 3) == 3);

    // Carry at most one output: a slow source (30 fps at x2 on 120 Hz, 4 outputs wanted, 2 shown) does not bank
    // credit that a faster source could spend as an overshoot.
    budget.Reset();
    double t = 0.0;
    for (int i = 0; i <= 60; ++i, t += 1.0 / 30.0)
        budget.Plan(t, 120, 1);
    CHECK(budget.Credit() <= 1.0 + 1e-9);
    t -= 1.0 / 30.0;
    uint64_t outputs = 0;
    const double fast = 1.0 / 200.0;
    for (int i = 1; i <= 200; ++i)
        outputs += 1 + budget.Plan(t + i * fast, 120, 1);
    // 200 real frames in 1 s at 200 fps need nothing generated once the interval average has caught up; what is
    // generated meanwhile comes from the average's lag plus the one carried output, not from banked credit.
    CHECK(outputs <= 200 + 12);
}

// 165 Hz with V-Sync, real rates whose multiples do not land on the refresh: integer N, fractional budget.
static void BudgetNonDivisor165()
{
    for (const double fps : { 60.0, 70.0 })
    {
        RefreshBudget budget;
        uint64_t generated = 0, outputs = 0;
        uint32_t lowest = 99, highest = 0;
        const uint32_t frames = 1650;
        for (uint32_t frame = 0; frame <= frames; ++frame)
        {
            const uint32_t g = budget.Plan(double(frame) / fps, 165, 3);
            generated += g;
            if (frame > 4)
            {
                outputs += 1 + g;
                lowest = std::min(lowest, g);
                highest = std::max(highest, g);
            }
        }
        const double seconds = double(frames) / fps;
        const double expected = 165.0 * seconds - double(frames); // the displayable remainder
        std::printf("budget %.0f fps x4 @165 Hz: %llu generated over %.1f s (%.2f per real, %u..%u), expected ~%.0f\n",
                    fps, (unsigned long long) generated, seconds, double(generated) / frames, lowest, highest,
                    expected);
        CHECK(std::abs(double(generated) - expected) <= expected * 0.01 + 2.0);
        CHECK(lowest >= 1 && highest <= 2); // 2.75 / 2.36 outputs per real frame: 1 or 2 generated, never 3
        CHECK(double(outputs) <= 165.0 * seconds + 2.0);
    }

    // V-Sync caps the target at the refresh; without V-Sync an FPS limit above it stands (XeFG_DynamicTargetFps).
    CHECK(TargetFps(0.0, 165.0, true) == 157.0);   // auto: just under the refresh, floor(165 - 165^2 / 3600)
    CHECK(TargetFps(200.0, 165.0, true) == 165.0); // min(target, refresh) under V-Sync
    CHECK(TargetFps(200.0, 165.0, false) == 200.0);
    CHECK(TargetFps(0.0, 0.0, true) == 0.0); // refresh unknown, no limit: no target

    // XeFG-style choice at 165 Hz V-Sync: the presented rate nearest the target, integer N in 2..4.
    AdviceInput in;
    in.refreshHz = 165.0;
    in.vsync = true;
    in.currentGenerated = 1;
    in.baseFps = 60.0; // x2 120, x3 180: 180 is nearer 157
    Advice a = Advise(in);
    CHECK(a.targetFps == 157.0);
    CHECK(a.xefgGenerated == 2);
    in.baseFps = 70.0; // x2 140, x3 210
    a = Advise(in);
    CHECK(a.xefgGenerated == 1);
    for (double base = 5.0; base <= 400.0; base += 0.5)
    {
        in.baseFps = base;
        a = Advise(in);
        CHECK(a.xefgGenerated >= 1 && a.xefgGenerated <= 3);
        CHECK(a.makoGenerated >= 1 && a.makoGenerated <= 3);
    }
}

// Interval trace at `ms` per frame, `count` frames, returning each observation.
static double Feed(CadenceEstimator& c, double& t, double ms, int count, double targetFps, Cadence* last = nullptr)
{
    Cadence r = Cadence::First;
    for (int i = 0; i < count; ++i)
    {
        t += ms / 1000.0;
        r = c.Observe(t, targetFps);
    }
    if (last)
        *last = r;
    return c.BaseFps();
}

static void CadenceBursty()
{
    // A frame-pacing-poor game alternating 8 / 32 ms reads its real rate (50 fps), not the mean of 125 and 31 fps.
    CadenceEstimator c;
    double t = 0.0;
    c.Observe(t, 120.0);
    double lo = 1e9, hi = 0.0;
    for (int i = 0; i < 400; ++i)
    {
        t += (i % 2 == 0 ? 8.0 : 32.0) / 1000.0;
        c.Observe(t, 120.0);
        if (i >= 10) // the first few intervals settle from the first sample (8 ms) toward the cadence
        {
            lo = std::min(lo, c.BaseFps());
            hi = std::max(hi, c.BaseFps());
        }
    }
    std::printf("cadence 8/32 ms: %.1f..%.1f fps (drops %llu, hitches %llu)\n", lo, hi,
                (unsigned long long) c.Counts().drops, (unsigned long long) c.Counts().hitches);
    CHECK(lo >= 40.0 && hi <= 60.0);
    CHECK(c.Counts().drops == 0);

    // A transient faster than 3x the baseline (and 2x the target) is excluded, the estimate does not move.
    CadenceEstimator b;
    t = 0.0;
    b.Observe(t, 120.0);
    const double before = Feed(b, t, 1000.0 / 60.0, 120, 120.0);
    Cadence r;
    const double during = Feed(b, t, 2.0, 5, 120.0, &r);
    CHECK(r == Cadence::Burst);
    CHECK(b.Counts().bursts == 5);
    CHECK(during == before);
    // 3x the baseline but under 2x the target is not a burst: 60 fps baseline, 200 fps instant, target 120.
    CadenceEstimator b2;
    t = 0.0;
    b2.Observe(t, 120.0);
    Feed(b2, t, 1000.0 / 60.0, 120, 120.0);
    Feed(b2, t, 5.0, 1, 120.0, &r); // 200 fps > 180 but < 240
    CHECK(r != Cadence::Burst);
}

static void CadenceHitchVsDrop()
{
    // One 50 ms hitch in 60 fps: not a cadence drop; the estimate comes back.
    CadenceEstimator h;
    double t = 0.0;
    h.Observe(t, 120.0);
    Feed(h, t, 1000.0 / 60.0, 120, 120.0);
    Cadence r;
    Feed(h, t, 50.0, 1, 120.0, &r);
    CHECK(r == Cadence::DropPending);
    Feed(h, t, 1000.0 / 60.0, 30, 120.0);
    std::printf("cadence hitch: %.1f fps after 30 frames (hitches %llu, drops %llu)\n", h.BaseFps(),
                (unsigned long long) h.Counts().hitches, (unsigned long long) h.Counts().drops);
    CHECK(h.Counts().drops == 0 && h.Counts().hitches == 1);
    CHECK(h.BaseFps() > 57.0 && h.BaseFps() < 61.0);

    // Two in a row are still a hitch; a third confirms a drop.
    CadenceEstimator two;
    t = 0.0;
    two.Observe(t, 120.0);
    Feed(two, t, 1000.0 / 60.0, 120, 120.0);
    Feed(two, t, 50.0, 2, 120.0);
    Feed(two, t, 1000.0 / 60.0, 1, 120.0);
    CHECK(two.Counts().drops == 0 && two.Counts().hitches == 1);

    // 60 -> 25 fps sustained: confirmed on the third slow frame, rebased at the new rate.
    CadenceEstimator d;
    t = 0.0;
    d.Observe(t, 120.0);
    Feed(d, t, 1000.0 / 60.0, 120, 120.0);
    Feed(d, t, 40.0, 2, 120.0, &r);
    CHECK(r == Cadence::DropPending);
    Feed(d, t, 40.0, 1, 120.0, &r);
    CHECK(r == Cadence::Drop);
    CHECK(d.Counts().drops == 1);
    CHECK(d.BaseFps() < 40.0); // the shadow estimate, already well off 60
    Feed(d, t, 40.0, 30, 120.0);
    std::printf("cadence 60->25 fps: %.1f fps after 33 slow frames (drops %llu)\n", d.BaseFps(),
                (unsigned long long) d.Counts().drops);
    CHECK(d.Counts().drops == 1);
    CHECK(d.BaseFps() > 24.0 && d.BaseFps() < 26.5);

    // Under 10 fps (or a clock going backwards) is a stall: excluded, the estimate is kept.
    CadenceEstimator s;
    t = 0.0;
    s.Observe(t, 120.0);
    const double kept = Feed(s, t, 1000.0 / 60.0, 60, 120.0);
    Feed(s, t, 150.0, 1, 120.0, &r);
    CHECK(r == Cadence::Stall && s.BaseFps() == kept && s.Counts().stalls == 1);
    CHECK(s.Observe(t - 0.01, 120.0) == Cadence::Stall);

    // Rebase skips an interval without a sample (an entry dropped before presentation).
    s.Rebase(t + 0.5);
    CHECK(s.BaseFps() == kept);
    CHECK(s.Observe(t + 0.5 + 1.0 / 60.0, 120.0) == Cadence::Sample);
}

static void Delivery()
{
    DeliveryWindow w;
    CHECK(w.Healthy()); // nothing requested
    w.Record(100, 95);
    CHECK(w.Healthy()); // 5% missed
    w.Reset();
    w.Record(100, 94);
    CHECK(!w.Healthy());
    w.Reset();
    w.Record(19, 18); // under 20 requested: complete delivery required
    CHECK(!w.Healthy());
    w.Reset();
    w.Record(10, 20); // more accepted than requested counts as requested
    CHECK(w.Healthy() && w.Accepted() == 10);
}

// MAKO adaptive_scheduler_tests.cpp:2990-3020: at a 120 fps target, x2 at 40 fps (80 out) to x3 at 30 fps (90 out)
// is a marginal trade and rejected; x3 at 32 fps (96 out) pays.
static void Acceptance()
{
    const LoadOutcome x2at40 = OutcomeAt(120.0, 1, 40.0);
    CHECK(x2at40.outputFps == 80.0);
    CHECK(!HigherLoadIsUseful(120.0, 2, x2at40, OutcomeAt(120.0, 2, 30.0), false));
    CHECK(HigherLoadIsUseful(120.0, 2, x2at40, OutcomeAt(120.0, 2, 32.0), false));

    // Output is capped at the target: past it, more frames buy nothing.
    CHECK(OutcomeAt(120.0, 3, 50.0).outputFps == 120.0);
    CHECK(!HigherLoadIsUseful(120.0, 2, OutcomeAt(120.0, 1, 60.0), OutcomeAt(120.0, 2, 59.0), false));

    // Delivered target evidence waives the real-frame cost (MAKO's reachesTarget), never for x2 or a projection.
    // x2 at 55 (110 out) to x3 delivering 120 at 45: +10 out for 10 real frames, short of the 2.4 margin, unless
    // the target was delivered from under it.
    CHECK(HigherLoadIsUseful(120.0, 2, OutcomeAt(120.0, 1, 55.0), { 45.0, 120.0 }, true));
    CHECK(!HigherLoadIsUseful(120.0, 2, OutcomeAt(120.0, 1, 55.0), { 45.0, 120.0 }, false));
    CHECK(!HigherLoadIsUseful(120.0, 2, OutcomeAt(120.0, 1, 60.0), { 45.0, 120.0 }, true)); // already met at x2

    // The adviser with evidence: x3 now at 30, x2 measured at 40 -> back to x2.
    AdviceInput in;
    in.limitFps = 120.0;
    in.refreshHz = 120.0;
    in.vsync = false;
    in.currentGenerated = 2;
    in.baseFps = 30.0;
    in.evidenceBaseFps[1] = 40.0;
    in.deliveryHealthy = true;
    Advice a = Advise(in);
    CHECK(a.targetFps == 120.0);
    CHECK(a.makoGenerated == 1 && a.makoWhy == Why::CurrentDoesNotPay);
    std::printf("advise x3@30 vs x2@40 @120: mako x%u (%s), xefg x%u (%s)\n", a.makoGenerated + 1, WhyText(a.makoWhy),
                a.xefgGenerated + 1, WhyText(a.xefgWhy));

    in.baseFps = 32.0;
    a = Advise(in);
    CHECK(a.makoGenerated == 2);

    // At x2 with x3 measured at 30: x3 does not pay, stay.
    in.currentGenerated = 1;
    in.baseFps = 40.0;
    in.evidenceBaseFps[1] = 0.0;
    in.evidenceBaseFps[2] = 30.0;
    a = Advise(in);
    CHECK(a.makoGenerated == 1 && a.makoWhy == Why::HigherDoesNotPay);
    in.evidenceBaseFps[2] = 33.0; // 99 out against 80: pays
    a = Advise(in);
    CHECK(a.makoGenerated == 2 && a.makoWhy == Why::HigherPays);

    // Nothing measured above: stay, and say a trial is needed; a projection never claims the step.
    in.evidenceBaseFps[2] = 0.0;
    a = Advise(in);
    CHECK(a.makoGenerated == 1 && a.makoWhy == Why::HigherUntested);

    // Target met at x2 (60 x 2 = 120): nothing to try.
    in.baseFps = 60.0;
    a = Advise(in);
    CHECK(a.makoGenerated == 1 && a.makoWhy == Why::TargetMet);

    // Delivery pressure (> 5% of generated frames missed): one step down, none below x2.
    in.currentGenerated = 2;
    in.baseFps = 40.0;
    in.deliveryHealthy = false;
    a = Advise(in);
    CHECK(a.makoGenerated == 1 && a.makoWhy == Why::DeliveryPressure);
    in.currentGenerated = 1;
    a = Advise(in);
    CHECK(a.makoGenerated == 1 && a.makoWhy == Why::DeliveryPressure);

    // No target (refresh unknown, no limit) or no cadence: keep what runs.
    AdviceInput none;
    none.currentGenerated = 3;
    none.baseFps = 40.0;
    a = Advise(none);
    CHECK(a.xefgGenerated == 3 && a.makoGenerated == 3 && a.xefgWhy == Why::NoTarget && a.makoWhy == Why::NoTarget);
    none.refreshHz = 120.0;
    none.baseFps = 0.0;
    a = Advise(none);
    CHECK(a.xefgGenerated == 3 && a.makoGenerated == 3 && a.xefgWhy == Why::NoCadence);
    none.currentGenerated = 9; // out of range in: clamped out
    none.baseFps = 40.0;
    a = Advise(none);
    CHECK(a.xefgGenerated <= 3 && a.makoGenerated <= 3 && a.xefgGenerated >= 1 && a.makoGenerated >= 1);

    for (int w = 0; w < (int) Why::Count; ++w)
        CHECK(WhyText(Why(w)) != nullptr && std::strlen(WhyText(Why(w))) > 0);
}

// The presenter thread's view: one OnEntry per presented entry, refresh sampled when due, a report per window.
static void ShadowWindows()
{
    Shadow s;

    // Refresh: due at once, then only every 2 s, and at a summary only if 2 s have passed.
    CHECK(s.RefreshDue(0.0, false));
    s.SetRefresh(0.0, 0); // unknown: retried after 2 s, not before
    CHECK(!s.RefreshDue(1.9, false));
    CHECK(s.RefreshDue(2.0, false));
    s.SetRefresh(2.0, 120);
    CHECK(!s.RefreshDue(30.0, false)); // known: only at a summary
    CHECK(!s.RefreshDue(3.0, true));
    CHECK(s.RefreshDue(4.0, true));

    // 10 s of 60 fps at x3 (2 generated), base indices contiguous: steady, healthy, evidence for x3.
    double t = 10.0;
    uint64_t base = 0;
    for (int i = 0; i < 600; ++i)
    {
        s.OnEntry(t, base, 3, 2, true);
        base += 3;
        t += 1.0 / 60.0;
    }
    WindowReport r = s.CloseWindow(t, 0.0);
    std::printf("shadow window: %.1f fps, %llu/%llu budgeted, mako x%u (%s), xefg x%u (%s)\n", r.baseFps,
                (unsigned long long) r.budgeted, (unsigned long long) r.generated, r.advice.makoGenerated + 1,
                WhyText(r.advice.makoWhy), r.advice.xefgGenerated + 1, WhyText(r.advice.xefgWhy));
    CHECK(r.refreshHz == 120 && r.vsync);
    CHECK(r.real == 600 && r.generated == 1200);
    CHECK(r.baseFps > 59.0 && r.baseFps < 61.0);
    CHECK(r.healthy && r.requested == 1200 && r.accepted == 1200);
    CHECK(r.current == 2 && r.steady);
    CHECK(r.budgeted <= r.generated);
    CHECK(r.budgeted >= 590 && r.budgeted <= 610); // 120 Hz shows one generated frame per real frame at 60 fps
    CHECK(r.advice.targetFps == 116.0);            // auto at 120 Hz, under V-Sync min(116, 120)
    CHECK(s.EvidenceFps(2) > 59.0);

    // Next window: counters start over; an entry dropped before presentation (3 skipped indices) is a delivery miss
    // and its interval is not a cadence sample.
    for (int i = 0; i < 100; ++i)
    {
        const uint64_t skipped = (i == 50) ? 3 : 0;
        base += skipped;
        s.OnEntry(t, base, 3, 2, true);
        base += 3;
        t += (skipped ? 2.0 : 1.0) / 60.0;
    }
    r = s.CloseWindow(t, 0.0);
    CHECK(r.real == 100 && r.generated == 200);
    CHECK(r.requested == 203 && r.accepted == 200);
    CHECK(r.healthy); // 3 missed of 203: within 5%
    CHECK(r.cadence.stalls == 0 && r.cadence.drops == 0);

    // Many entries dropped: unhealthy, and the window is no evidence; the earlier x3 evidence stays.
    const double x3 = s.EvidenceFps(2);
    for (int i = 0; i < 100; ++i)
    {
        const uint64_t skipped = (i % 5 == 0) ? 3 : 0;
        base += skipped;
        s.OnEntry(t, base, 3, 2, true);
        base += 3;
        t += (skipped ? 2.0 : 1.0) / 45.0;
    }
    r = s.CloseWindow(t, 0.0);
    CHECK(r.requested == 260 && r.accepted == 200 && !r.healthy && !r.steady);
    CHECK(s.EvidenceFps(2) == x3);
    CHECK(r.advice.makoWhy == Why::DeliveryPressure && r.advice.makoGenerated == 1);

    // A window that changed multiplier is not steady either; the last entry's count is what runs.
    for (int i = 0; i < 200; ++i)
    {
        const uint32_t g = i < 100 ? 2 : 1;
        s.OnEntry(t, base, g + 1, g, true);
        base += g + 1;
        t += 1.0 / 50.0;
    }
    r = s.CloseWindow(t, 0.0);
    CHECK(!r.steady && r.current == 1 && r.healthy);
    CHECK(s.EvidenceFps(1) == 0.0);

    // A steady x2 window at 50 fps records x2 evidence; the next x3 window at 30 fps is told x3 does not pay.
    for (int i = 0; i < 500; ++i)
    {
        s.OnEntry(t, base, 2, 1, false);
        base += 2;
        t += 1.0 / 50.0;
    }
    r = s.CloseWindow(t, 120.0);
    CHECK(r.steady && s.EvidenceFps(1) > 49.0 && s.EvidenceFps(1) < 51.0);
    for (int i = 0; i < 300; ++i)
    {
        s.OnEntry(t, base, 3, 2, false);
        base += 3;
        t += 1.0 / 30.0;
    }
    r = s.CloseWindow(t, 120.0);
    CHECK(r.advice.targetFps == 120.0 && r.advice.makoGenerated == 1 && r.advice.makoWhy == Why::CurrentDoesNotPay);

    // Evidence older than 60 s is dropped: a later scene is not judged by it.
    t += 61.0;
    for (int i = 0; i < 300; ++i)
    {
        s.OnEntry(t, base, 3, 2, false);
        base += 3;
        t += 1.0 / 30.0;
    }
    r = s.CloseWindow(t, 120.0);
    CHECK(r.advice.makoGenerated == 2);

    // A window with no entries reports nothing and recommends what runs.
    r = s.CloseWindow(t + 10.0, 120.0);
    CHECK(r.real == 0 && !r.steady);
}

int main()
{
    BudgetMakoVectors();
    BudgetGuards();
    BudgetNonDivisor165();
    CadenceBursty();
    CadenceHitchVsDrop();
    Delivery();
    Acceptance();
    ShadowWindows();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
