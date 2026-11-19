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
                check(parked.empty(), "good members are not retired on allocation failure");
                const std::array<ID3D12Resource*, 4> good { g_nr.output, g_nr.colorCopy, g_nr.hdrCopy,
                                                            g_nr.colorSmall };
                const int attempts = allocations;
                failAt = 0;
                for (frameNumber = 1; frameNumber < 8; ++frameNumber)
                {
                    check(!scratch(chain, reduced), "refuse until backoff expires");
                    check(allocations == attempts, "backoff makes no allocation attempts");
                }
                check(scratch(chain, reduced), "partial scratch allocation recovers after backoff");
                const std::array<ID3D12Resource*, 4> recovered { g_nr.output, g_nr.colorCopy, g_nr.hdrCopy,
                                                                 g_nr.colorSmall };
                for (unsigned i = 0; i < good.size(); ++i)
                    check(!good[i] || good[i] == recovered[i], "successful members survive retry");
                check(allocations == attempts + 1 && parked.empty(), "retry allocates only missing member");
                check(g_nr.output && g_nr.colorCopy && g_nr.hdrCopy && (!reduced || g_nr.colorSmall),
                      "complete scratch after retry");
            }
        }
    // A permanently failing colorSmall must not allocate/retire three good textures each frame.
    fresh();
    failFrom = 4;
    for (frameNumber = 0; frameNumber < 1000; ++frameNumber)
    {
        check(!scratch(false, true), "persistent OOM always refuses NR");
        check(!g_nr.failed && g_nr.reset, "persistent OOM does not latch or lose reset");
    }
    check(failedFrames == std::vector<uint64_t> { 0, 8, 24, 56, 120, 248, 488, 728, 968 },
          "exponential retry capped at 240 frames");
    check(allocations == 12 && resources.size() == 3 && parked.empty(),
          "persistent OOM bounds work and retains good bundle");
    failFrom = 0;
    for (; frameNumber < 1208; ++frameNumber)
        check(!scratch(false, true), "recovery waits for scheduled retry");
    check(scratch(false, true) && allocations == 13, "recovery allocates only colorSmall");
    g_nr.colorSmall = nullptr;
    failAt = 14;
    ++frameNumber;
    check(!scratch(false, true), "new failure after success");
    failAt = 0;
    frameNumber += 7;
    check(!scratch(false, true) && allocations == 14, "successful bundle reset backoff to eight");
    ++frameNumber;
    check(scratch(false, true) && allocations == 15, "new failure recovers after eight frames");

    // A partial bundle can predate the feature, and output itself can be missing on a format change.
    fresh();
    failAt = 1;
    check(!scratch(false, true), "output allocation fails before feature exists");
    failAt = 0;
    frameNumber = 1;
    check(scratch(false, true, 1920, 1080, 960, 540, 3),
          "format change immediately retries partial output-less bundle");
    check(parked.size() == 3 && g_nr.colorCopy->GetDesc().Format == 3 && g_nr.hdrCopy->GetDesc().Format == 3 &&
              g_nr.colorSmall->GetDesc().Format == 3,
          "only stale-format members retired");
    fresh();
    failAt = 1;
    check(!scratch(true, true), "partial bundle before deferred chain admission");
    failAt = 0;
    frameNumber = 1;
    deferScratch = true;
    check(!scratch(true, true, 1920, 1080, 960, 540, 3), "new contract can be deferred before allocation");
    deferScratch = false;
    ++frameNumber;
    check(scratch(true, true, 1920, 1080, 960, 540, 3), "deferred contract retries");
    check(parked.size() == 3 && g_nr.colorCopy->GetDesc().Format == 3 && g_nr.hdrCopy->GetDesc().Format == 3 &&
              g_nr.colorSmall->GetDesc().Format == 3,
          "deferred validation must not mark old members compatible");
    for (unsigned axis = 0; axis < 4; ++axis)
    {
        fresh();
        failAt = 4;
        check(!scratch(false, true), "colorSmall allocation fails before feature exists");
        auto* color = g_nr.colorCopy;
        auto* hdr = g_nr.hdrCopy;
        auto* output = g_nr.output;
        unsigned w = 1920, h = 1080, ww = 960, wh = 540;
        if (axis == 0)
            ++w;
        if (axis == 1)
            ++h;
        if (axis == 2)
            ++ww;
        if (axis == 3)
            ++wh;
        failAt = 0;
        frameNumber = 1;
        check(scratch(false, true, w, h, ww, wh), "any extent change immediately resets retry backoff");
        check(g_nr.output->GetDesc().Width == ww && g_nr.output->GetDesc().Height == wh &&
                  g_nr.colorCopy->GetDesc().Width == w && g_nr.hdrCopy->GetDesc().Height == h,
              "resized partial bundle fits contract");
        check(axis < 2 ? g_nr.output == output : (g_nr.colorCopy == color && g_nr.hdrCopy == hdr),
              "compatible members survive extent change");
        check(parked.size() == (axis < 2 ? 2u : 1u), "retire only incompatible members");
    }
    fresh();
    frameNumber = 100;
    failAt = 4;
    check(!scratch(false, true), "counted frame fails");
    failAt = 0;
    for (unsigned repeat = 0; repeat < 100; ++repeat)
        check(!scratch(false, true), "repeated same-frame calls do not spend backoff");
    check(allocations == 4, "one attempt per counted frame");
    frameNumber = 1;
    check(scratch(false, true), "clock rollback cannot strand retry");
    fresh();
    failAt = 4;
    check(!scratch(false, true), "unobserved frame fails");
    failAt = 0;
    for (unsigned call = 1; call < 8; ++call)
        check(!scratch(false, true), "no-frame-counter fallback bounded");
    check(scratch(false, true), "no-frame-counter route eventually recovers");

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
