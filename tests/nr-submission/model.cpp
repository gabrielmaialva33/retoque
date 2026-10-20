#include "retoque/dlssnr/DlssNr_SubmissionModel.h"
#include <cassert>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace DlssNr::Submission::Detail;

int main()
{
    std::array<uint64_t, MaxQueues> fences {};
    auto completed = [&](size_t queue) { return fences[queue]; };
    auto submit = [](Model& m, uintptr_t list, size_t queue, uint64_t value, bool success = true)
    {
        auto e = m.BeforeExecute(list);
        assert(e);
        Model::AfterExecute(e, queue, value, success);
    };
    unsigned cases = 0;
    {
        Model m;
        Usage use;
        auto e = m.Track(1, use, completed);
        assert(e && !IsComplete(*e, completed) && !IsReady(*e, completed));
        auto snapshot = use;
        assert(snapshot.epochs.back() == e);
        Model::AfterReset(e, true);
        assert(!IsReady(*e, completed)); // missing/delayed submit never becomes proof
        ++cases;
    }
    {
        Model m;
        Usage use;
        auto e = m.Track(1, use, completed);
        submit(m, 1, 0, 1);
        fences[0] = 1;
        assert(IsComplete(*e, completed) && !IsReady(*e, completed));
        auto pending = m.BeforeExecute(1); // duplicate execute invalidates prior completion
        assert(!IsComplete(*e, completed));
        Model::AfterExecute(pending, 0, 2, true);
        assert(!IsComplete(*e, completed));
        Model::AfterReset(e, false);
        fences[0] = 2;
        assert(IsComplete(*e, completed) && !IsReady(*e, completed));
        Model::AfterReset(e, true);
        assert(IsReady(*e, completed));
        ++cases;
    }
    {
        Model m;
        Usage use;
        auto e = m.Track(2, use, completed);
        submit(m, 2, 0, 3);
        submit(m, 2, 1, 7);
        Model::AfterReset(e, true);
        fences[0] = 3;
        fences[1] = 6;
        assert(!IsReady(*e, completed));
        fences[1] = 7;
        assert(IsReady(*e, completed));
        fences[1] = UINT64_MAX;
        assert(!IsReady(*e, completed));
        fences[1] = 7;
        ++cases;
    }
    {
        Model m;
        Usage use;
        auto old = m.Track(3, use, completed);
        auto pending = m.BeforeExecute(3);
        Model::AfterReset(old, true); // reset after execute but before Signal notification
        assert(!IsReady(*old, completed));
        auto fresh = m.Track(3, use, completed);
        assert(fresh != old && !fresh->submitted);
        Model::AfterExecute(pending, 0, 9, true);
        assert(!IsReady(*old, completed) && !fresh->submitted);
        fences[0] = 9;
        assert(IsReady(*old, completed) && !IsComplete(*fresh, completed));
        ++cases;
    }
    {
        Model m;
        Usage use;
        auto e = m.Track(4, use, completed);
        submit(m, 4, 0, 10, false);
        assert(!m.Track(4, use, completed));
        Model::AfterReset(e, true);
        fences[0] = 100;
        assert(!IsReady(*e, completed));
        ++cases;
    }
    {
        Model m;
        Usage use;
        for (size_t i = 1; i <= MaxUses; ++i)
            assert(m.Track(i, use, completed));
        assert(!m.Track(MaxUses + 1, use, completed));
        // A checkpoint cannot lose unresolved uses under pressure.
        for (const auto& e : use.epochs)
            assert(e && !e->sealed);
        ++cases;
    }
    {
        Model m;
        std::array<Usage, MaxEpochs / MaxUses + 1> uses {};
        for (size_t i = 0; i < MaxEpochs; ++i)
            assert(m.Track(i + 1, uses[i / MaxUses], completed));
        assert(!m.Track(MaxEpochs + 1, uses.back(), completed));
        ++cases;
    }
    {
        Model m;
        Usage use;
        auto e = m.Track(7, use, completed);
        assert(!IsSubmittedOnlyOn(*e, 0)); // not submitted yet
        submit(m, 7, 0, 15);
        assert(IsSubmittedOnlyOn(*e, 0));  // submitted only on queue 0
        assert(!IsSubmittedOnlyOn(*e, 1)); // not on queue 1
        submit(m, 7, 1, 20);
        assert(!IsSubmittedOnlyOn(*e, 0)); // submitted on multiple queues
        assert(!IsSubmittedOnlyOn(*e, 1));
        m.Abandon(7);
        assert(m.Track(7, use, completed) != nullptr); // epoch recycled
        ++cases;
    }
    {
        Model m;
        Usage use;
        for (uint64_t frame = 1; frame <= 3000; ++frame)
        {
            auto e = m.Track(5, use, completed);
            assert(e);
            submit(m, 5, 0, frame);
            Model::AfterReset(e, true);
            fences[0] = frame;
            assert(IsReady(*e, completed));
        }
        ++cases;
    }
    {
        // Actual concurrent reset/notify interleavings under the production lock discipline.
        Model m;
        Usage use;
        std::mutex lock;
        for (uint64_t frame = 1; frame <= 1000; ++frame)
        {
            auto e = m.Track(6, use, completed);
            auto pending = m.BeforeExecute(6);
            std::thread reset(
                [&]
                {
                    std::lock_guard guard(lock);
                    Model::AfterReset(e, true);
                });
            std::thread notify(
                [&]
                {
                    std::lock_guard guard(lock);
                    Model::AfterExecute(pending, 2, frame, true);
                });
            reset.join();
            notify.join();
            fences[2] = frame;
            assert(IsReady(*e, completed));
        }
        ++cases;
    }
    {
        Model m;
        Usage use;
        auto e = m.Track(7, use, completed);
        assert(!IsSubmittedOnlyOn(*e, 0)); // not submitted yet
        submit(m, 7, 0, 15);
        assert(IsSubmittedOnlyOn(*e, 0));  // submitted only on queue 0
        assert(!IsSubmittedOnlyOn(*e, 1)); // not on queue 1
        submit(m, 7, 1, 20);
        assert(!IsSubmittedOnlyOn(*e, 0)); // now submitted on queues 0 and 1, so not only on 0
        assert(!IsSubmittedOnlyOn(*e, 1));
        ++cases;
    }
    // docs/dlssnr/design/chain-in-flight.md
    // Overlap established (one queue for AffinityBatches tracked batches): the production Batch calls NoteQueue,
    // these cases do not, so they pass it.
    const auto admit = [&](const Usage& u, const Model& m, uintptr_t list)
    { return Admit(u, completed, m.Clock(), m.Strict(), !m.MultiQueue(), list); };
    const auto held = [](const Usage& u, const std::shared_ptr<Epoch>& e)
    {
        for (const auto& x : u.epochs)
            if (x && x == e)
                return true;
        return false;
    };
    {
        // Complete but never Reset: done for admission at once, finished for freeing only after the replay window;
        // strict needs Ready for both.
        fences = {};
        Model m;
        Usage use;
        auto e = m.Track(20, use, completed);
        submit(m, 20, 0, 3);
        assert(!IsAdmissionDone(*e, completed, false));
        fences[0] = 3;
        assert(IsAdmissionDone(*e, completed, false) && !IsAdmissionDone(*e, completed, true));
        assert(!IsFinished(*e, completed, m.Clock(), false) && !IsSettled(use, completed, m.Clock(), false));
        m.Advance(AgeOutTicks);
        assert(IsFinished(*e, completed, m.Clock(), false) && !IsFinished(*e, completed, m.Clock(), true));
        assert(IsSettled(use, completed, m.Clock(), false) && !IsSettled(use, completed, m.Clock(), true));
        ++cases;
    }
    {
        // A valid fence still outstanding is NEVER finished, however old: ticks are not destruction evidence.
        fences = {};
        Model m;
        Usage use;
        auto e = m.Track(21, use, completed);
        submit(m, 21, 0, 5);
        m.Advance(10 * AgeOutTicks);
        assert(!IsFinished(*e, completed, m.Clock(), false) && !IsUnanswerable(*e));
        assert(!IsSettled(use, completed, m.Clock(), false));
        assert(!IsOnlyUnanswerableLeft(use, completed, m.Clock(), false));
        assert(Admit(use, completed, m.Clock(), false, true, 99, 0) == Admission::Behind);
        fences[0] = 5;
        assert(IsSettled(use, completed, m.Clock(), false));
        ++cases;
    }
    {
        // A recording waiting for its first submission: fine for RingTicks, then nothing new is admitted (the rings
        // would wrap over it). Never settled; stuck-exempt once aged. A late submission makes it count normally.
        fences = {};
        Model m;
        Usage use;
        auto late = m.Track(22, use, completed);
        m.Advance(RingTicks - 1);
        assert(admit(use, m, 23) == Admission::Admit);
        m.Advance(RingTicks);
        assert(admit(use, m, 23) == Admission::Pending);
        m.Advance(AgeOutTicks);
        assert(admit(use, m, 23) == Admission::Pending); // age never excuses a recording that has not run
        assert(!IsSettled(use, completed, m.Clock(), false) &&
               IsOnlyUnanswerableLeft(use, completed, m.Clock(), false));
        submit(m, 22, 0, 8);
        assert(!IsUnanswerable(*late) && !IsOnlyUnanswerableLeft(use, completed, m.Clock(), false));
        assert(admit(use, m, 23) == Admission::Behind); // submitted at last, and still RingTicks old: not done
        fences[0] = 8;
        assert(admit(use, m, 23) == Admission::Admit);
        m.Advance(2 * AgeOutTicks);
        assert(IsSettled(use, completed, m.Clock(), false));
        ++cases;
    }
    {
        // The game released the list (only our reference left): an unsubmitted recording can never run, a completed
        // one can never replay. Safe in any mode, even strict.
        fences = {};
        Model m;
        Usage use;
        auto never = m.Track(24, use, completed);
        auto done = m.Track(25, use, completed);
        submit(m, 25, 0, 1);
        fences[0] = 1;
        m.Advance(RingTicks);
        assert(admit(use, m, 26) == Admission::Pending);
        never->orphaned = done->orphaned = true;
        assert(IsSafe(*never, completed) && IsSafe(*done, completed) && IsSettled(use, completed, m.Clock(), true));
        assert(admit(use, m, 26) == Admission::Admit);
        auto running = m.Track(27, use, completed);
        submit(m, 27, 0, 2);
        running->orphaned = true;
        assert(!IsSafe(*running, completed)); // released, but the GPU is not done with it
        ++cases;
    }
    {
        // A failed Signal keeps protecting its ring slots (Codex, fourth review): Behind, never admitted past, and
        // never settled.
        fences = {};
        Model m;
        Usage use;
        m.Track(28, use, completed);
        submit(m, 28, 0, 0, false);
        assert(Admit(use, completed, m.Clock(), false, true, 29, 0) == Admission::InFlight);
        m.Advance(AgeOutTicks);
        assert(Admit(use, completed, m.Clock(), false, true, 29) == Admission::Behind);
        assert(!IsSettled(use, completed, m.Clock(), false) &&
               IsOnlyUnanswerableLeft(use, completed, m.Clock(), false));
        ++cases;
    }
    {
        // Recorded, then Reset without executing: cancelled, safe in either mode, pruned.
        fences = {};
        Model m;
        Usage use;
        auto e = m.Track(30, use, completed);
        Model::AfterReset(e, true);
        assert(IsCancelled(*e) && IsSafe(*e, completed) && IsSettled(use, completed, m.Clock(), true));
        assert(held(use, e));
        PruneLifetime(use, completed, m.Clock(), false);
        assert(!held(use, e));
        ++cases;
    }
    {
        // At most MaxEarlierInFlight unfinished, and never a second recording into an unsubmitted list.
        fences = {};
        Model m;
        Usage use;
        for (uintptr_t list = 31; list < 31 + MaxEarlierInFlight; ++list)
        {
            m.Track(list, use, completed);
            submit(m, list, 0, list);
        }
        assert(admit(use, m, 40) == Admission::Admit);
        m.Track(40, use, completed);
        assert(admit(use, m, 40) == Admission::SameList);
        submit(m, 40, 0, 40);
        assert(admit(use, m, 41) == Admission::InFlight);
        fences[0] = 31;
        assert(admit(use, m, 41) == Admission::Admit);
        ++cases;
    }
    {
        // Overlap only once one queue is established; a switch, or an unknown queue after a known one, serializes for
        // the session. NoteQueue reports the queue switched from; nothing is injected to order the two.
        fences = {};
        Model m;
        Usage use;
        m.Track(50, use, completed);
        submit(m, 50, 0, 1);
        for (uint64_t i = 1; i < AffinityBatches; ++i)
        {
            assert(m.NoteQueue(0) == MaxQueues && !m.Overlap());
            assert(Admit(use, completed, m.Clock(), false, m.Overlap(), 51) == Admission::Serial);
        }
        assert(m.NoteQueue(0) == MaxQueues && m.Overlap());
        assert(Admit(use, completed, m.Clock(), false, m.Overlap(), 51) == Admission::Admit);
        m.Track(51, use, completed);
        assert(m.NoteQueue(1) == 0 && m.MultiQueue() && !m.Overlap());
        submit(m, 51, 1, 1);
        assert(Admit(use, completed, m.Clock(), false, m.Overlap(), 52) == Admission::Serial);
        fences[0] = fences[1] = 1;
        assert(Admit(use, completed, m.Clock(), false, m.Overlap(), 52) == Admission::Admit); // serial, not stopped
        // Mixed unfinished queues are refused even with overlap.
        Model mixed;
        Usage two;
        fences = {};
        mixed.Track(53, two, completed);
        submit(mixed, 53, 0, 1);
        mixed.Track(54, two, completed);
        submit(mixed, 54, 1, 1);
        assert(Admit(two, completed, mixed.Clock(), false, true, 55) == Admission::Queues);
        Model unknown;
        assert(unknown.NoteQueue(MaxQueues) == MaxQueues && unknown.MultiQueue() && !unknown.Overlap());
        Model later;
        assert(later.NoteQueue(3) == MaxQueues);
        assert(later.NoteQueue(MaxQueues) == 3 && later.MultiQueue());
        ++cases;
    }
    {
        // The ring bound holds through completion, not just submission (Codex round 3): a recording held back for
        // seven entries and then submitted to a queue that is behind still stops new work at RingTicks.
        fences = {};
        Model m;
        Usage use;
        auto held = m.Track(56, use, completed);
        for (uintptr_t list = 57; list < 57 + 7; ++list)
        {
            m.Advance(list - 56);
            assert(admit(use, m, list) == Admission::Admit);
            m.Track(list, use, completed);
            submit(m, list, 0, list);
            fences[0] = list; // these complete at once
        }
        submit(m, 56, 0, 100); // A finally submitted, behind on the GPU
        m.Advance(RingTicks);
        assert(admit(use, m, 70) == Admission::Behind);
        fences[0] = 100;
        assert(admit(use, m, 70) == Admission::Admit);
        (void) held;
        ++cases;
    }
    {
        // Replay inside the window: the epoch is still in the lifetime set AND objects parked on it are not yet
        // freed; strict mode then waits for Ready.
        fences = {};
        Model m;
        Usage use;
        auto a = m.Track(60, use, completed);
        submit(m, 60, 0, 1);
        fences[0] = 1;
        PruneLifetime(use, completed, m.Clock(), m.Strict());
        Usage parked = use; // a retirement snapshot
        assert(held(use, a) && admit(use, m, 61) == Admission::Admit);
        m.Advance(AgeOutTicks / 2);
        assert(!IsSettled(parked, completed, m.Clock(), m.Strict())); // still inside the window
        submit(m, 60, 0, 2);                                          // executed again without a Reset
        assert(m.Strict() && admit(use, m, 61) == Admission::Replay);
        fences[0] = 2;
        m.Advance(4 * AgeOutTicks);
        assert(admit(use, m, 61) == Admission::Replay && !IsSettled(parked, completed, m.Clock(), m.Strict()));
        Model::AfterReset(a, true);
        assert(admit(use, m, 61) == Admission::Admit && IsSettled(parked, completed, m.Clock(), m.Strict()));
        ++cases;
    }
    {
        // Outside the window a completed, unsealed epoch leaves the lifetime set.
        fences = {};
        Model m;
        Usage use;
        auto e = m.Track(62, use, completed);
        submit(m, 62, 0, 1);
        fences[0] = 1;
        m.Advance(AgeOutTicks - 1);
        PruneLifetime(use, completed, m.Clock(), m.Strict());
        assert(held(use, e));
        m.Advance(AgeOutTicks);
        PruneLifetime(use, completed, m.Clock(), m.Strict());
        assert(!held(use, e));
        ++cases;
    }
    {
        // One persistent Usage, a fresh never-reset list per entry, each completed at once: capacity pressure prunes
        // the oldest admission-done epochs, so recording never stops for want of a slot (outside strict mode).
        fences = {};
        Model m;
        Usage use;
        for (uintptr_t list = 3000; list < 3000 + 4 * MaxUses; ++list)
        {
            m.Advance(list);
            PruneLifetime(use, completed, m.Clock(), m.Strict());
            assert(admit(use, m, list) == Admission::Admit);
            assert(m.Track(list, use, completed));
            submit(m, list, 0, list);
            fences[0] = list;
        }
        ++cases;
    }
    {
        // The same recording twice in one batch is a replay too.
        fences = {};
        Model m;
        Usage use;
        auto e = m.Track(63, use, completed);
        auto first = m.BeforeExecute(63);
        m.DuplicateInBatch(e);
        Model::AfterExecute(first, 0, 1, true);
        assert(m.Strict() && e->executions == 2 && e->pending == 0);
        ++cases;
    }
    {
        // A game that never resets its lists cannot fill the tracker: completed, unheld, aged epochs are reclaimed.
        static_assert(AgeOutTicks < MaxEpochs);
        fences = {};
        Model m;
        for (uintptr_t list = 1000; list < 1000 + 4 * MaxEpochs; ++list)
        {
            m.Advance(list);
            Usage use;
            assert(m.Track(list, use, completed));
            submit(m, list, 0, list);
            fences[0] = list;
        }
        // Held by a Usage: never reclaimed; refuse rather than evict.
        Model full;
        Usage hold;
        std::vector<Usage> holds(MaxEpochs);
        fences = {};
        for (size_t i = 0; i < MaxEpochs; ++i)
        {
            assert(full.Track(5000 + i, holds[i], completed));
            submit(full, 5000 + i, 0, 1);
        }
        fences[0] = 1;
        full.Advance(AgeOutTicks);
        assert(!full.Track(9999, hold, completed));
        holds.clear();
        assert(full.Track(9999, hold, completed));
        // Never reclaimed in strict mode unless Ready.
        Model strict;
        std::vector<Usage> keep(MaxEpochs);
        fences = {};
        for (size_t i = 0; i < MaxEpochs; ++i)
        {
            strict.Track(7000 + i, keep[i], completed);
            submit(strict, 7000 + i, 0, 1);
        }
        submit(strict, 7000, 0, 2); // replay
        fences[0] = 2;
        strict.Advance(AgeOutTicks);
        keep.clear();
        assert(strict.Strict() && !strict.Track(9998, hold, completed));
        ++cases;
    }
    std::cout << "PASS: " << cases
              << " submission lifetime cases; 3000 retirements; 1000 concurrent reset/notify pairs\n";
}
