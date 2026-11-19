int main()
{
    for (bool chain : { false, true })
        for (bool reduced : { false, true })
        {
            for (int missing = 1; missing <= (reduced ? 4 : 3); ++missing)
            {
                fresh();
                failAt = missing;
                check(!scratch(chain, reduced), "incomplete scratch refuses every route");
                check(!reason.empty() && fakeDevice.releases == 1,
                      "scratch refusal reported and device reference returned");
                check(!g_nr.failed && g_nr.reset, "scratch refusal transient with history reset");
                check(!g_nr.output && !g_nr.colorCopy && !g_nr.hdrCopy && !g_nr.colorSmall,
                      "partial generation parked before another size/format can retry");
                check(parked.size() == resources.size(), "all acquired scratch retired without releasing it");
                failAt = 0;
                check(scratch(chain, reduced), "partial scratch allocation recovers");
                check(g_nr.output && g_nr.colorCopy && g_nr.hdrCopy && (!reduced || g_nr.colorSmall),
                      "complete scratch after retry");
            }
        }
    for (int fault = 0; fault < 5; ++fault)
    {
        fresh();
        if (fault == 0)
            shaderAvailable = false;
        if (fault == 1)
            initOk = false;
        if (fault == 2)
            failAt = 1;
        if (fault == 3)
            failAt = 2;
        if (fault == 4)
            dispatchOk = false;
        check(!guides(true, shaderAvailable), "needed resample failure refuses model");
        check(evaluations == 0 && barriers == 0, "refused guides never reach evaluate or read transition");
        check(!reason.empty() && fakeDevice.releases == 1, "guide refusal reported and device reference returned");
        check(g_nr.reset && !g_nr.failed, "guide refusal requests reset without permanent latch");
        for (bool reset : g_nr.passReset)
            check(reset, "every chain history reset after gap");
        auto* pass = g_nr.guideMatch;
        auto* depth = g_nr.depthMatched;
        auto* motion = g_nr.motionMatched;
        shaderAvailable = initOk = dispatchOk = true;
        failAt = 0;
        check(guides(true, true), "transient resample failure recovers next frame");
        check(evaluations == 1 && barriers == 2, "recovery uses matched guides once");
        check(!depth || depth == g_nr.depthMatched, "matched depth retained");
        check(!motion || motion == g_nr.motionMatched, "matched motion retained");
        if (fault == 4)
            check(pass == g_nr.guideMatch && destroys == 0, "initialized pass survives dispatch refusal");
        if (fault == 1)
            check(destroys == 1, "only never-initialized pass rebuilt");
    }
    fresh();
    check(guides(false, false), "MatchGuides off retains game contract");
    check(allocations == 0 && dispatches == 0 && !g_nr.guideMatch, "off allocates and dispatches nothing");
    fresh();
    check(guides(true, false, 1920, 1080), "native-sized guides need no shader");
    check(allocations == 0 && dispatches == 0 && !g_nr.guideMatch, "unneeded resample allocates nothing");
    fresh();
    if (failures)
        return 1;
    std::puts("PASS: scratch failures on single/chain at native/reduced sizes; guide allocation/init/dispatch "
              "failures, retries and game bypass");
}
