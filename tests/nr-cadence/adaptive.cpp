#include <dlssnr/DlssNr_CadenceAdaptive.h>
#include <dlssnr/DlssNr_GpuTimingModel.h>
#include <cassert>
#include <limits>
#include <cstdio>
#include <optional>
#include <vector>
using namespace DlssNr::Cadence;
using namespace DlssNr::Cadence::Adaptive;
// Model the producer/consumer timing: one eligible evaluation records one probe,
// polling happens first, and each slot waits for its own simulated certificate.
static std::vector<uint32_t> DelayedFeedback(uint64_t delay, uint64_t mfg)
{
    Controller controller;
    std::array<std::optional<Sample>, 4> pending {};
    std::vector<uint32_t> intervals;
    uint64_t gamePresents = 0;
    for (uint64_t evaluation = 1; evaluation <= 130; ++evaluation)
    {
        gamePresents += mfg;
        assert(gamePresents == evaluation * mfg);
        const int64_t now = static_cast<int64_t>(evaluation * 16);
        std::array<Sample, 4> ready {};
        size_t count = 0;
        for (auto& slot : pending)
        {
            if (slot && evaluation - slot->present >= delay)
            {
                auto sample = *slot;
                if (sample.present == 30 || sample.present == 45)
                {
                    // The production settlement/certificate policy: cancelled unsubmitted recording,
                    // then a completed orphan without Reset. Both free the slot but discard its data.
                    DlssNr::Submission::Detail::Usage usage {};
                    auto epoch = std::make_shared<DlssNr::Submission::Detail::Epoch>();
                    usage.epochs[0] = epoch;
                    if (sample.present == 30)
                        epoch->sealed = true;
                    else
                    {
                        epoch->orphaned = true;
                        epoch->submitted = true;
                        epoch->executions = 1;
                        epoch->fences[0] = 1;
                    }
                    auto completed = [](size_t) { return uint64_t { 1 }; };
                    const auto certificate = DlssNr::GpuTiming::Detail::CertifyUsage(usage, completed, [](size_t)
                                                                                     { return uint64_t { 1000 }; });
                    assert(!certificate.reusable && !certificate.accepted);
                    assert(DlssNr::Submission::Detail::IsSettled(usage, completed, evaluation, true));
                    sample.valid = false;
                }
                ready[count++] = sample;
                slot.reset(); // certificate.reusable or Settled, never just age
            }
        }
        std::sort(ready.begin(), ready.end(), [](const Sample& a, const Sample& b) { return a.present < b.present; });
        for (const auto& sample : ready)
            controller.Observe(sample, evaluation, now);
        const auto interval = controller.Interval(4, evaluation, now);
        intervals.push_back(interval);
        if (evaluation == 29 || evaluation == 75)
            assert(interval == 4); // >300ms calm; recovers after discarded capture 30
        if (evaluation == 30 + delay || evaluation == 45 + delay || evaluation == 80 + delay)
            assert(interval == 1); // discarded sample / first sample showing motion
        if (evaluation == 100 + delay + 5)
            assert(interval == 1); // no fresh feedback, even though capture gap is bounded separately
        if (evaluation >= 100 && evaluation <= 110)
            continue; // no free/eligible probe during a gap
        bool recorded = false;
        for (auto& slot : pending)
        {
            if (slot)
                continue;
            slot = Sample { evaluation, now, evaluation == 80 ? 2.0f : 0.1f, 0.01f, true };
            recorded = true;
            break;
        }
        assert(recorded); // four slots cover four evaluations of certification latency
    }
    return intervals;
}

int main()
{
    for (uint64_t delay : { 2u, 3u, 4u })
        assert(DelayedFeedback(delay, 1) == DelayedFeedback(delay, 4));
    Controller c;
    assert(c.Interval(4, 1, 0) == 1);
    for (uint64_t i = 1; i <= 21; ++i)
        c.Observe({ i, static_cast<int64_t>(i * 16), 0.1f, 0.01f, true }, i + 1, i * 16 + 1);
    assert(c.Interval(4, 22, 337) == 4);
    c.Observe({ 22, 352, 0.5f, 0.01f, true }, 23, 353);
    assert(c.Interval(4, 23, 353) == 2);
    c.Observe({ 23, 368, 1.0f, 0.01f, true }, 24, 369);
    assert(c.Interval(4, 24, 369) == 1);
    for (uint64_t i = 24; i <= 44; ++i)
        c.Observe({ i, static_cast<int64_t>(i * 16), 0.1f, 0.01f, true }, i + 1, i * 16 + 1);
    assert(c.Interval(3, 45, 705) == 3);
    assert(c.Interval(3, 49, 705) == 1);  // stale by presents
    assert(c.Interval(3, 45, 1000) == 1); // stale by clock
    c.Observe({ 45, 720, 0.1f, 0.15f, true }, 46, 721);
    assert(c.Interval(4, 46, 721) == 1);
    c.Observe({ 45, 720, 0.0f, 0.0f, true }, 46, 721); // duplicate cannot restore calm
    assert(c.Interval(4, 46, 721) == 1);
    c.Reset();
    c.Observe({ 1, 10, 0.0f, 0.0f, true }, 1, 11); // same present unproven
    assert(c.Interval(4, 2, 12) == 1);
    c.Observe({ 2, 20, std::numeric_limits<float>::quiet_NaN(), 0, true }, 3, 21);
    assert(c.Interval(4, 3, 21) == 1);
    std::array<Pixel, 256> grid {};
    for (auto& p : grid)
        p = { 0.5f, 0.1f, 1.0f, 0 };
    auto s = Summarize(grid, 1, 1);
    assert(s.valid && s.motion == 0.5f && s.rejection > 0.099f);
    grid[0].valid = -1;
    assert(!Summarize(grid, 1, 1).valid);
    grid = {};
    assert(!Summarize(grid, 1, 1).valid);
    // A long feedback gap must re-arm calm, even if the last accepted interval was four.
    c.Reset();
    for (uint64_t i = 1; i <= 21; ++i)
        c.Observe({ i, static_cast<int64_t>(i * 16), 0.0f, 0.0f, true }, i + 1, i * 16 + 1);
    assert(c.Interval(4, 22, 337) == 4);
    c.Observe({ 100, 2000, 0.0f, 0.0f, true }, 101, 2001);
    assert(c.Interval(4, 101, 2001) == 1);
    // Changing the proposed interval must restart the calm window.
    c.Reset();
    for (uint64_t i = 1; i <= 19; ++i)
        c.Observe({ i, static_cast<int64_t>(i * 16), 0.25f, 0.0f, true }, i + 1, i * 16 + 1);
    c.Observe({ 20, 320, 0.0f, 0.0f, true }, 21, 321);
    assert(c.Interval(4, 21, 321) == 1);
    c.Observe({ 21, 336, 0.0f, 0.0f, false }, 22, 337);
    assert(c.Interval(4, 22, 337) == 1);

    // Completions from older captures must never roll back a newer decision, including discarded slots.
    c.Reset();
    for (uint64_t i = 1; i <= 21; ++i)
        c.Observe({ i, static_cast<int64_t>(i * 16), 0.0f, 0.0f, true }, i + 1, i * 16 + 1);
    c.Observe({ 20, 320, 2.0f, 1.0f, false }, 22, 337);
    assert(c.Interval(4, 22, 337) == 4);
    c.Observe({ 22, 352, 2.0f, 0.0f, true }, 23, 353);
    c.Observe({ 21, 336, 0.0f, 0.0f, true }, 23, 353);
    assert(c.Interval(4, 23, 353) == 1);

    Inputs in {};
    in.requested = 4;
    in.adaptiveInterval = 1;
    in.smoothing = true;
    Scheduler scheduler;
    Signature signature {};
    for (unsigned i = 1; i < 10; ++i)
    {
        auto d = scheduler.Decide(in, signature, i, i * 16, false);
        assert(d.runModel && d.record && !d.fade && d.cadence == 1);
        scheduler.Recorded();
    }
    in.adaptiveInterval = 2;
    assert(!scheduler.Decide(in, signature, 10, 160, false).runModel);
    scheduler.Carried();
    in.adaptiveInterval = 1;
    auto d = scheduler.Decide(in, signature, 11, 176, false);
    assert(d.runModel && d.handChain && !d.fade);
    std::puts("PASS: adaptive reduction, thresholds, calm/recovery, stale/invalid feedback and interval-1 scheduling");
}
