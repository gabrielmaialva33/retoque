// Present route split: four timestamps [route begin, before Dispatch, after Dispatch, route end] on one recording.
// Uses the real GpuTiming.cpp with the external D3D12/Submission boundary above; runs in its own process.
int RegionSplitCases()
{
    unsigned cases = 0;
    Device device;
    std::array<List, 16> lists;
    for (auto& list : lists)
        list.device = &device;
    using GpuTiming::Region;
    using GpuTiming::RegionKind;
    GpuTiming::RegionMetadata metadata;
    metadata.guides = GpuTiming::GuideSource::Game;
    uint64_t fence = 0;
    const auto complete = [&](List& list)
    {
        Execute(list);
        Seal(list);
        completed[0] = fence += 10;
        GpuTiming::Poll();
    };
    const auto near = [](double value, double expected) { return value > expected - 1e-6 && value < expected + 1e-6; };

    // 1 MHz queue: one tick is 0.001 ms. Prefix 10, Dispatch 100, suffix 20.
    tickSteps = { 10, 10, 100, 20 };
    {
        Region route(&lists[0], RegionKind::Present, metadata, true, 1);
        route.BeforeDispatch();
        route.AfterDispatch();
        route.Finish(true);
    }
    complete(lists[0]);
    auto split = GpuTiming::GetSnapshot().present;
    assert(split.valid && split.dispatchMarked);
    assert(near(split.totalMs, 0.130) && near(split.dispatchCallMs, 0.100) && near(split.outsideDispatchMs, 0.030));
    ++cases; // The split comes from one recording: route = Dispatch call + outside, nothing borrowed.

    tickSteps = { 10, 0, 100, 0 };
    {
        Region route(&lists[1], RegionKind::Present, metadata, true, 1);
        route.BeforeDispatch();
        route.AfterDispatch();
        route.Finish(true);
    }
    complete(lists[1]);
    split = GpuTiming::GetSnapshot().present;
    assert(split.sampleId == 2 && split.dispatchMarked && near(split.totalMs, 0.100) &&
           near(split.dispatchCallMs, 0.100) && near(split.outsideDispatchMs, 0.0));
    ++cases; // Zero prefix and suffix around a real Dispatch is a valid sample, not "no evidence".

    tickSteps = { 10, 40 };
    {
        Region route(&lists[2], RegionKind::Present, metadata, true, 1);
        route.Finish(true);
    }
    complete(lists[2]);
    split = GpuTiming::GetSnapshot().present;
    assert(split.sampleId == 3 && !split.dispatchMarked && near(split.totalMs, 0.040) && split.dispatchCallMs == 0.0 &&
           split.outsideDispatchMs == 0.0);
    ++cases; // A route that never reached Dispatch publishes its total only, without inventing a split.

    const auto beforeBroken = GpuTiming::GetSnapshot().present;
    const auto mapsBeforeBroken = maps;
    {
        Region route(&lists[3], RegionKind::Present, metadata, true, 1);
        route.BeforeDispatch(); // Dispatch's end never marked.
        route.Finish(true);
    }
    complete(lists[3]);
    {
        Region route(&lists[4], RegionKind::Present, metadata, true, 1);
        route.AfterDispatch(); // End without a begin.
        route.Finish(true);
    }
    complete(lists[4]);
    {
        Region route(&lists[5], RegionKind::Present, metadata, true, 1);
        route.BeforeDispatch();
        route.BeforeDispatch(); // Twice.
        route.AfterDispatch();
        route.Finish(true);
    }
    complete(lists[5]);
    {
        Region capture(&lists[6], RegionKind::GuideCapture, metadata, true, 1);
        capture.BeforeDispatch(); // A capture has no Dispatch inside it.
        capture.AfterDispatch();
        capture.Finish(true);
    }
    complete(lists[6]);
    auto broken = GpuTiming::GetSnapshot();
    assert(maps == mapsBeforeBroken && broken.present.sampleId == beforeBroken.sampleId &&
           broken.present.discarded == beforeBroken.discarded + 3 && broken.capture.discarded == 1 &&
           !broken.capture.valid && broken.present.pending == 0 && broken.capture.pending == 0);
    ++cases; // Every malformed split is discarded unmapped and recycled; the last good sample stays.

    tickSteps = { 10, 5, 30, 50, 20, 5 }; // T0, T1, [Evaluation begin, end], T2, T3
    {
        Region route(&lists[7], RegionKind::Present, metadata, true, 1);
        route.BeforeDispatch();
        {
            GpuTiming::Metadata inner;
            inner.carried = true;
            inner.cadence = 2;
            char stage[] = "after";
            inner.stage = stage;
            GpuTiming::Evaluation evaluation(&device, &lists[7], inner, true, 1);
            evaluation.FinishOnScopeExit(0, true);
        }
        route.AfterDispatch();
        route.Finish(true);
    }
    complete(lists[7]);
    const auto nested = GpuTiming::GetSnapshot();
    assert(nested.present.dispatchMarked && near(nested.present.totalMs, 0.110) &&
           near(nested.present.dispatchCallMs, 0.100) && near(nested.present.outsideDispatchMs, 0.010));
    assert(nested.valid && near(nested.totalMs, 0.050) && nested.accepted == 1);
    ++cases; // The Evaluation inside Dispatch keeps its own heap and sample; neither is added to the other.

    tickSteps.clear();
    {
        Region off(&lists[8], RegionKind::Present, metadata, false, 1);
        off.BeforeDispatch();
        off.AfterDispatch();
        off.Finish(true);
    }
    assert(GpuTiming::GetSnapshot().present.eligible == nested.present.eligible);
    ++cases; // Marks on a disabled or unsampled region record nothing.

    if (cases != 6)
        return 2;
    std::cout << "PASS " << cases << " production Present route split cases\n";
    return 0;
}
