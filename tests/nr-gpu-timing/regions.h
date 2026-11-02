// Uses the real GpuTiming.cpp with the external D3D12/Submission boundary above.
int RegionCases()
{
    unsigned cases = 0;
    Device device;
    std::array<List, 40> lists;
    for (auto& list : lists)
        list.device = &device;
    using GpuTiming::GuideSource;
    using GpuTiming::Region;
    using GpuTiming::RegionKind;
    GpuTiming::RegionMetadata metadata;
    metadata.presentId = 90;
    metadata.guideSequence = 17;
    metadata.guideAgeMs = 4;
    metadata.guides = GuideSource::Game;
    metadata.outputWidth = 3840;
    metadata.outputHeight = 2160;
    metadata.nrRecorded = true;
    {
        Region off(&lists[0], RegionKind::Present, metadata, false, 1);
        off.SetMetadata(metadata);
        off.Finish(true);
    }
    assert(heaps == 0 && buffers == 0 && tracks == 0 && queries == 0 && maps == 0);
    ++cases; // Disabled must be inert.

    {
        Region present(&lists[0], RegionKind::Present, metadata, true, 1);
        GpuTiming::Conversion conversion(&device, &lists[0], true, 1);
        conversion.Begin();
        conversion.End();
        Record(device, lists[0]);
        conversion.Begin();
        conversion.End();
        conversion.Finish(true);
        present.SetMetadata(metadata);
        metadata.guideSequence = 999;
        metadata.guides = GuideSource::Zero;
        present.Finish(true);
        present.Finish(false); // Finish and the destructor must be idempotent.
    }
    assert(queries == 10 && resolves == 3);
    GpuTiming::Poll();
    assert(maps == 0);
    Execute(lists[0]);
    GpuTiming::Poll();
    assert(maps == 0);
    Seal(lists[0]);
    GpuTiming::Poll();
    assert(maps == 0 && GpuTiming::GetSnapshot().present.waitingGpu == 1);
    ++cases; // Neither execution nor sealing alone proves completion.
    completed[0] = 10;
    GpuTiming::Poll();
    auto first = GpuTiming::GetSnapshot();
    assert(maps == 3 && first.accepted == 1 && first.conversionValid && first.present.accepted == 1);
    assert(first.present.valid && first.present.totalMs > 0.089999 && first.present.totalMs < 0.090001);
    assert(first.present.metadata.guideSequence == 17 && first.present.metadata.guides == GuideSource::Game);
    assert(first.present.metadata.presentId == 90 && first.present.metadata.guideAgeMs == 4);
    assert(first.eligible == 1 && first.present.eligible == 1 && first.capture.eligible == 0);
    ++cases; // Nested scopes own distinct queries and immutable metadata.

    {
        Region capture(&lists[1], RegionKind::GuideCapture, metadata, true, 1);
        capture.Finish(true);
    }
    frequencies[1] = 2000000;
    Execute(lists[1], 1);
    Seal(lists[1]);
    GpuTiming::Poll();
    assert(maps == 3);
    completed[1] = 10;
    GpuTiming::Poll();
    auto separate = GpuTiming::GetSnapshot();
    assert(separate.capture.valid && separate.capture.totalMs > 0.004999 && separate.capture.totalMs < 0.005001 &&
           separate.present.sampleId == 1);
    assert(separate.capture.accepted == 1 && separate.accepted == 1);
    ++cases; // Producer queue frequency and counters remain independent.

    completed[1] = 0;
    for (unsigned i = 2; i <= 3; ++i)
    {
        metadata.presentId = i;
        Region timing(&lists[i], RegionKind::Present, metadata, true, 1);
        timing.Finish(true);
        Execute(lists[i], i == 2 ? 1 : 0);
        Seal(lists[i]);
    }
    GpuTiming::Poll();
    assert(GpuTiming::GetSnapshot().present.sampleId == 3);
    completed[1] = 10;
    GpuTiming::Poll();
    assert(GpuTiming::GetSnapshot().present.sampleId == 3 && GpuTiming::GetSnapshot().present.metadata.presentId == 3 &&
           GpuTiming::GetSnapshot().present.accepted == 3);
    ++cases; // An older recording completing later cannot overwrite the latest.

    const auto beforeCadenceQueries = queries;
    {
        Region present(&lists[4], RegionKind::Present, metadata, true, 3);      // eligible 4: sampled
        Region capture(&lists[4], RegionKind::GuideCapture, metadata, true, 3); // eligible 2: skipped
        present.Finish(true);
        capture.Finish(true);
    }
    assert(queries == beforeCadenceQueries + 2 && GpuTiming::GetSnapshot().capture.sampled == 1);
    Execute(lists[4]);
    Seal(lists[4]);
    GpuTiming::Poll();
    assert(GpuTiming::GetSnapshot().present.sampleInterval == 3);
    ++cases; // Per-kind cadence cannot borrow another producer's eligible count.

    const auto beforeAbort = GpuTiming::GetSnapshot();
    {
        Region abandoned(&lists[5], RegionKind::Present, metadata, true, 1);
    }
    Execute(lists[5]);
    Seal(lists[5]);
    const auto mapsBeforeAbort = maps;
    GpuTiming::Poll();
    auto afterAbort = GpuTiming::GetSnapshot();
    assert(maps == mapsBeforeAbort && afterAbort.present.discarded == beforeAbort.present.discarded + 1 &&
           afterAbort.discarded == beforeAbort.discarded);
    ++cases; // Scope failure must never publish a successful route.

    {
        Region replay(&lists[6], RegionKind::Present, metadata, true, 1);
        replay.Finish(true);
    }
    Execute(lists[6]);
    Execute(lists[6]);
    Seal(lists[6]);
    GpuTiming::Poll();
    assert(maps == mapsBeforeAbort && GpuTiming::GetSnapshot().present.pending == 0);
    ++cases; // Replayed recordings are discarded without mapping and safely recycled.

    const auto queriesBeforeFailure = queries;
    failTrack = true;
    {
        Region refused(&lists[7], RegionKind::GuideCapture, metadata, true, 1);
        refused.Finish(true);
    }
    failTrack = false;
    assert(queries == queriesBeforeFailure && GpuTiming::GetSnapshot().capture.pending == 0);
    ++cases; // No timestamp can be recorded without a tracking ticket.

    {
        Region unmappable(&lists[8], RegionKind::GuideCapture, metadata, true, 1);
        unmappable.Finish(true);
    }
    Execute(lists[8]);
    Seal(lists[8]);
    failMap = true;
    GpuTiming::Poll();
    failMap = false;
    assert(GpuTiming::GetSnapshot().capture.sampleId == 1 && GpuTiming::GetSnapshot().capture.pending == 0);
    ++cases; // A Map failure keeps the previously confirmed sample.

    const auto beforePool = GpuTiming::GetSnapshot();
    for (unsigned i = 10; i < 19; ++i)
    {
        Region held(&lists[i], RegionKind::Present, metadata, true, 1);
        held.Finish(true);
    }
    const auto full = GpuTiming::GetSnapshot();
    assert(full.present.sampled == beforePool.present.sampled + 8 &&
           full.present.discarded == beforePool.present.discarded + 1 && full.present.pending == 8);
    Record(device, lists[19]);
    {
        Region capture(&lists[20], RegionKind::GuideCapture, metadata, true, 1);
        capture.Finish(true);
    }
    assert(GpuTiming::GetSnapshot().sampled == beforePool.sampled + 1 &&
           GpuTiming::GetSnapshot().capture.sampled == beforePool.capture.sampled + 1);
    ++cases; // A full Present pool cannot starve Dispatch or guide capture.

    GpuTiming::SetEnabled(false);
    const auto freesWhilePending = heapFrees;
    assert(GpuTiming::GetSnapshot().present.pending == 8);
    for (unsigned i : { 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 19u, 20u })
    {
        Execute(lists[i]);
        Seal(lists[i]);
    }
    GpuTiming::Poll();
    assert(heapFrees == freesWhilePending + 10 && device.references == 1);
    assert(GpuTiming::GetSnapshot().present.pending == 0 && GpuTiming::GetSnapshot().capture.pending == 0);
    ++cases; // Disable retains pending resources, then releases only certified safe ones.

    {
        Region lost(&lists[21], RegionKind::Present, metadata, true, 1);
        lost.Finish(true);
    }
    Execute(lists[21]);
    Seal(lists[21]);
    completed[0] = UINT64_MAX;
    const auto beforeLossMaps = maps;
    GpuTiming::Poll();
    GpuTiming::SetEnabled(false);
    const auto lost = GpuTiming::GetSnapshot();
    assert(maps == beforeLossMaps && lost.present.pending == 1 && lost.present.retained == 1);
    ++cases; // Device removal is not successful completion and cannot free uncertain work.

    assert(cases == 13 && queries > 0 && maps > 0);
    std::cout << "PASS " << cases << " production Present/capture region timing cases\n";
    return 0;
}
