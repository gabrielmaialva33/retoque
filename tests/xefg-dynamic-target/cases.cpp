
static int failures = 0;

static void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// The production method, with the inputs it reads set
static float Target(float refresh, bool vsync, float set, float limit)
{
    Config::Instance()->FGXeFGFramerateTargetDMFG.value = set;
    Config::Instance()->FramerateLimit.value = limit;
    State::Instance().fgPresentVsync.store(vsync);
    XeFG_DynamicMFG xefg;
    xefg.refresh = refresh;
    const float target = xefg.TargetFps(1000.0, nullptr);
    Check(xefg.refreshQueries <= 1, "TargetFps asks for the refresh rate at most once");
    return target;
}

struct TargetCase
{
    float refresh;
    bool vsync;
    float set;   // FramerateTargetDMFG, 0 = auto
    float limit; // FramerateLimit, 0 = none
    float expected;
    const char* what;
};

static void CheckTargets()
{
    struct Display
    {
        float refresh, autoTarget, above, below, limitAbove, limitBelow;
    };

    // Auto is just under the refresh rate: floor(r - r * r / 3600)
    const Display displays[] = {
        { 60.0f, 59.0f, 90.0f, 50.0f, 75.0f, 45.0f },
        { 120.0f, 116.0f, 180.0f, 100.0f, 144.0f, 90.0f },
        { 165.0f, 157.0f, 240.0f, 140.0f, 200.0f, 120.0f },
    };

    for (const auto& d : displays)
    {
        const float r = d.refresh;
        const TargetCase cases[] = {
            { r, false, 0.0f, 0.0f, d.autoTarget, "auto, V-Sync off: just under the refresh rate" },
            { r, true, 0.0f, 0.0f, d.autoTarget, "auto, V-Sync on: just under the refresh rate" },
            { r, false, d.above, 0.0f, d.above, "target above the refresh, V-Sync off: as set" },
            { r, true, d.above, 0.0f, r, "target above the refresh, V-Sync on: the refresh rate" },
            { r, false, d.below, 0.0f, d.below, "target below the refresh, V-Sync off: as set" },
            { r, true, d.below, 0.0f, d.below, "target below the refresh, V-Sync on: as set" },
            { r, true, r, 0.0f, r, "target at the refresh, V-Sync on: the refresh rate" },
            { r, false, 0.0f, d.limitAbove, d.limitAbove,
              "auto with an FPS cap above the refresh, V-Sync off: the cap" },
            { r, true, 0.0f, d.limitAbove, r, "auto with an FPS cap above the refresh, V-Sync on: the refresh rate" },
            { r, true, 0.0f, d.limitBelow, d.limitBelow, "auto with an FPS cap below the refresh, V-Sync on: the cap" },
            { r, false, d.above, d.limitAbove, d.limitAbove, "target and cap above the refresh, V-Sync off: the cap" },
            { r, true, d.above, d.limitAbove, r, "target and cap above the refresh, V-Sync on: the refresh rate" },
            { r, true, d.above, d.limitBelow, d.limitBelow, "target above, cap below the refresh, V-Sync on: the cap" },
        };

        for (const auto& c : cases)
        {
            const float got = Target(c.refresh, c.vsync, c.set, c.limit);
            if (got != c.expected)
                std::printf("  at %.0f Hz (set %.0f, cap %.0f, V-Sync %s): %.1f, expected %.1f\n", c.refresh, c.set,
                            c.limit, c.vsync ? "on" : "off", got, c.expected);
            Check(got == c.expected, c.what);
        }
    }

    // Refresh rate unknown (0): nothing to hold the target to
    const TargetCase unknown[] = {
        { 0.0f, false, 0.0f, 0.0f, 0.0f, "unknown refresh, auto, V-Sync off: no target" },
        { 0.0f, true, 0.0f, 0.0f, 0.0f, "unknown refresh, auto, V-Sync on: no target" },
        { 0.0f, true, 180.0f, 0.0f, 180.0f, "unknown refresh, target, V-Sync on: as set" },
        { 0.0f, true, 0.0f, 144.0f, 144.0f, "unknown refresh, FPS cap, V-Sync on: the cap" },
        { 0.0f, true, 180.0f, 144.0f, 144.0f, "unknown refresh, target above the cap, V-Sync on: the cap" },
    };

    for (const auto& c : unknown)
    {
        const float got = Target(c.refresh, c.vsync, c.set, c.limit);
        if (got != c.expected)
            std::printf("  refresh unknown (set %.0f, cap %.0f, V-Sync %s): %.1f, expected %.1f\n", c.set, c.limit,
                        c.vsync ? "on" : "off", got, c.expected);
        Check(got == c.expected, c.what);
    }
}

// A simulated game for both production controllers: 30 ms of work per real frame, the first generated frame
// costs 1 ms (frame generation on) and each further one 0.5 ms. With V-Sync XeFG drops the generated frames that
// don't fit the refresh rather than holding the game, so the real frame rate is the game's own and (count + 1)
// times it reads above the refresh. That premise is janblade's observation (Witcher 3, 120 Hz), not measured here.
static double WorkMs(int count) { return 30.0 + (count > 0 ? 1.0 + (count - 1) * 0.5 : 0.0); }

struct Settled
{
    int count = 0;
    int lateChanges = 0;
};

static Settled SimCount(double targetFps, double refreshFps)
{
    XeFG_DynamicCount controller;
    Settled s;
    int count = 2;
    double now = 0.0;
    constexpr double seconds = 120.0;

    while (now < seconds * 1000.0)
    {
        const double frameMs = WorkMs(count);
        now += frameMs;
        const int next =
            controller.Update({ now, frameMs, 0.0, targetFps, refreshFps, 0.0, 5, count, 1 /* nearest */, 1 });
        if (next != count && now >= seconds * 500.0)
            ++s.lateChanges;
        count = next;
    }

    s.count = count;
    return s;
}

// "Use target as cap": OptiScaler's limiter holds the presented rate at the target (FrameLimit::targetFps)
static Settled SimCap(double targetFps)
{
    XeFG_DynamicCap controller;
    Settled s;
    int count = 2;
    double now = 0.0;
    constexpr double seconds = 120.0;

    while (now < seconds * 1000.0)
    {
        const double work = WorkMs(count);
        const double frameMs = std::max(work, (count + 1) * 1000.0 / targetFps);
        now += frameMs;
        const int next = controller.Update({ now, frameMs, frameMs - work, targetFps, 5, count });
        if (next != count && now >= seconds * 500.0)
            ++s.lateChanges;
        count = next;
    }

    s.count = count;
    return s;
}

static void CheckControllers()
{
    // 120 Hz with V-Sync, FramerateTargetDMFG 180. 3X shows 3 x 31.7 = 95 fps, 4X 4 x 31.3 = 125 fps: 4X is the
    // fewest generated frames that fill the refresh rate.
    const double target = Target(120.0f, true, 180.0f, 0.0f);
    const auto asSet = SimCount(180.0, 120.0);
    const auto followed = SimCount(target, 120.0);
    std::printf("  120 Hz, V-Sync, target 180: the count controller settles at %dX as set, %dX at the target %.0f\n",
                asSet.count + 1, followed.count + 1, target);
    Check(asSet.count == 5, "control: aimed at 180 the count controller climbs to 6X for frames V-Sync drops");
    Check(followed.count == 3, "the count controller settles at 4X, the fewest that fill 120 Hz");
    Check(followed.lateChanges <= 2, "the count controller holds its count");

    const auto capAsSet = SimCap(180.0);
    const auto capFollowed = SimCap(target);
    std::printf("  with Use target as cap: %dX as set, %dX at the target %.0f\n", capAsSet.count + 1,
                capFollowed.count + 1, target);
    Check(capAsSet.count == 5, "control: capped at 180 the cap controller climbs to 6X");
    Check(capFollowed.count == 3, "the cap controller settles at 4X");
    Check(capFollowed.lateChanges <= 2, "the cap controller holds its count");
}

#ifdef HAVE_PRESENT_SLICE
// FGPresent's final sync interval, what XeFG presents with, is what the target reads as V-Sync
static void CheckPresentSignal()
{
    auto* config = Config::Instance();
    auto& state = State::Instance();
    config->VsyncInterval.value = 0;

    struct Case
    {
        std::optional<bool> force;
        UINT gameInterval;
        bool expected;
        const char* what;
    };

    const Case cases[] = {
        { std::nullopt, 1, true, "the game's interval 1: V-Sync" },
        { std::nullopt, 2, true, "the game's interval 2: V-Sync" },
        { std::nullopt, 0, false, "the game's interval 0: no V-Sync" },
        { true, 0, true, "ForceVsync=true over the game's 0: V-Sync" },
        { false, 1, false, "ForceVsync=false over the game's 1: no V-Sync" },
    };

    for (const auto& c : cases)
    {
        config->ForceVsync = c.force;
        state.fgPresentVsync.store(!c.expected);
        Check(PresentVsync(c.gameInterval, 0, true) == c.expected, c.what);
    }

    // A DXGI_PRESENT_TEST call presents nothing and leaves the last answer
    config->ForceVsync.reset();
    state.fgPresentVsync.store(true);
    Check(PresentVsync(0, 0, false), "a test present leaves the V-Sync state as it was");
}
#endif

int main()
{
    CheckTargets();
    CheckControllers();
#ifdef HAVE_PRESENT_SLICE
    CheckPresentSignal();
#endif
    std::printf(failures == 0 ? "xefg-dynamic-target: PASS\n" : "xefg-dynamic-target: FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
