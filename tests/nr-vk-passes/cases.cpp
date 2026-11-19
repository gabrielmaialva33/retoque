#include "pch.h"
#include "DlssNr_VkChain.h"
#include <cassert>
#include <iostream>
#include <limits>

using namespace DlssNr;
using namespace std::chrono_literals;

int main()
{
    using namespace VkChain;
    Prerequisites ready { true, true, true, true, true };
    for (Route route : { Route::AfterUpscale, Route::BeforeUpscale, Route::Present, Route::Proxy })
    {
        assert(Plan(1, route, {}).legacy);
        assert(Plan(1, route, {}).count == 1);
        if (route != Route::AfterUpscale)
            assert(Plan(10, route, ready).refusal == Refusal::Route);
    }
    assert(Plan(10, Route::AfterUpscale, ready).count == 10);
    assert(Plan(999, Route::AfterUpscale, ready).count == 10);
    assert(Plan(0, Route::AfterUpscale, {}).legacy);
    for (unsigned missing = 0; missing < 5; ++missing)
    {
        auto prerequisites = ready;
        switch (missing)
        {
        case 0:
            prerequisites.completionObserved = false;
            break;
        case 1:
            prerequisites.serialRecording = false;
            break;
        case 2:
            prerequisites.orderedQueue = false;
            break;
        case 3:
            prerequisites.budgetAvailable = false;
            break;
        case 4:
            prerequisites.primaryRecording = false;
            break;
        }
        const auto plan = Plan(2, Route::AfterUpscale, prerequisites);
        assert(plan.refusal != Refusal::None && plan.count == 0 && !plan.legacy);
    }

    const VkRecording::Ticket ticket { 1, 2, 3, VkRecording::Reuse::Serial };
    Creation creation(ticket, 20);
    assert(!creation.Ready(21));           // elapsed presents are not submission
    assert(!creation.Complete(ticket, 7)); // a completion without successful submission
    assert(creation.Submitted(ticket, 7));
    assert(!creation.Ready(1000000));
    assert(!creation.Complete({ 2, 2, 3 }, 7)); // wrong device
    assert(!creation.Complete({ 1, 9, 3 }, 7)); // wrong buffer
    assert(!creation.Complete({ 1, 2, 4 }, 7)); // reused native handle, different epoch
    assert(!creation.Complete(ticket, 6));      // recycled fence / stale serial
    assert(creation.Complete(ticket, 7));
    assert(!creation.Ready(20)); // even completed initialization waits a later present
    assert(creation.Ready(21));
    assert(!creation.Submitted(ticket, 8)); // v1 refuses replay, does not preserve stale completion
    assert(!creation.Ready(22));
    assert(!creation.Complete(ticket, 8));

    for (bool submitted : { false, true })
    {
        Creation cancelled(ticket, 1);
        if (submitted)
            assert(cancelled.Submitted(ticket, 1));
        cancelled.Invalidate(); // discard, submit failure, observer loss or lost device
        assert(!cancelled.Submitted(ticket, 2));
        assert(!cancelled.Complete(ticket, 1));
        assert(!cancelled.Ready(std::numeric_limits<uint64_t>::max()));
    }
    for (auto invalid : { VkRecording::Ticket {}, VkRecording::Ticket { 1, 2, 0 },
                          VkRecording::Ticket { 1, 2, 3, VkRecording::Reuse::Unknown },
                          VkRecording::Ticket { 1, 2, 3, VkRecording::Reuse::Simultaneous } })
    {
        Creation unknown(invalid, 1);
        assert(!unknown.Submitted(invalid, 1));
        assert(!unknown.Ready(2));
    }
    Creation noPresent(ticket, 0);
    assert(!noPresent.Submitted(ticket, 1));
    Creation noSerial(ticket, 1);
    assert(!noSerial.Submitted(ticket, 0));
    assert(!noSerial.Ready(2));

    // Exercise the REAL registry, not a replacement model: retired pins cannot prove a creation executed.
    VkRecording::Registry recordings;
    recordings.Allocate(1, 10, { 2 });
    recordings.Begin(2, VkRecording::Reuse::Serial);
    auto pin = std::make_shared<int>(0);
    auto current = *recordings.Current(1, 2);
    Creation discarded(current, 1);
    assert(recordings.Pin(current, { pin }));
    assert(pin.use_count() == 2);
    recordings.End(2);
    recordings.Reset(2); // never submitted
    assert(pin.use_count() == 1);
    assert(!discarded.Ready(100));
    recordings.Begin(2, VkRecording::Reuse::Serial);
    auto replacement = *recordings.Current(1, 2);
    assert(replacement.epoch != current.epoch);
    assert(!discarded.Submitted(replacement, 1));
    Creation pending(replacement, 2);
    assert(recordings.Pin(replacement, { pin }));
    assert(pending.Submitted(replacement, 2));
    recordings.End(2);
    recordings.Reset(2); // the registry has no submission/completion callback to answer this
    assert(pin.use_count() == 1 && !pending.Ready(100));
    assert(!pending.Submitted(replacement, 3)); // a second submit while pending invalidates v1
    assert(!pending.Complete(replacement, 2));

    constexpr uint64_t MiB = 1ull << 20;
    constexpr uint64_t GiB = 1ull << 30;
    assert(!Admit(0, 0, 0, 0));
    assert(!Admit(8 * GiB, 8 * GiB, 0, 0));
    assert(!Admit(8 * GiB, 9 * GiB, 0, 0));
    assert(!Admit(8 * GiB, 7 * GiB, GiB, 0));
    assert(Admit(24 * GiB, 8 * GiB, GiB, 100 * MiB));
    assert(Admit(1024 * MiB, 128 * MiB, 0, 0)); // 512 reserve + 384 model, exact fit
    assert(!Admit(1024 * MiB, 128 * MiB + 1, 0, 0));
    const auto huge = std::numeric_limits<uint64_t>::max();
    assert(!Admit(huge, 0, huge, 0));
    assert(!Admit(huge, 0, 0, huge));
    assert(!Admit(huge, huge - 1, huge, huge));

    FailureLatch failure;
    DlssNrResolvedPassSettings tuning;
    assert(!failure.Refused(tuning));
    failure.Fail(tuning);
    assert(failure.Refused(tuning));
    auto changed = tuning;
    changed.Intensity = 0.25f;
    assert(!failure.Refused(changed));
    assert(failure.Refused(tuning)); // querying a changed value cannot erase the refusal
    failure.ClearAfterRetirement();
    assert(!failure.Refused(tuning));

    // Shared D3D12 decisions, including all ten stopping points and immutable original input.
    for (unsigned count = 2; count <= PassConfig::MaxPasses; ++count)
        for (unsigned fail = 0; fail <= count; ++fail)
        {
            Chain::Routing<int> route(1, 2, 3);
            for (unsigned pass = 0; pass < count; ++pass)
            {
                assert(route.Output() != route.original && route.Output() != route.Input());
                if (pass == fail)
                    break;
                route.Success();
            }
            assert(route.completed == fail);
            assert(route.answer == (fail == 0 ? 1 : fail % 2 ? 2 : 3));
        }
    Chain::Schedule schedule;
    DlssNrPassSnapshot settings;
    settings.Count = 10;
    const auto start = std::chrono::steady_clock::time_point(1s);
    assert(!schedule.Stable(settings, start));
    assert(!schedule.Stable(settings, start + 499ms));
    assert(schedule.Stable(settings, start + 500ms));
    assert(schedule.Create(1, start));
    assert(!schedule.Create(1, start + 1s));
    assert(!schedule.Create(2, start + 499ms));
    assert(schedule.Create(2, start + 500ms));
    assert(schedule.Evaluate(1));
    assert(!schedule.Evaluate(1));
    settings.Settings[9].Intensity = 0.25f;
    assert(!schedule.Stable(settings, start + 1s));
    assert(schedule.Stable(settings, start + 1500ms));
    std::cout << "PASS: Vulkan chain decisions, creation evidence, real recording retirement, budget and routing\n";
}
