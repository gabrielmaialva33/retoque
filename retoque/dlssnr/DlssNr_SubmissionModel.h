#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <limits>

namespace DlssNr::Submission::Detail
{
// Caller serializes this model. No callbacks into the renderer and no GPU assumptions.
constexpr size_t MaxEpochs = 256;
constexpr size_t MaxUses = 64;
constexpr size_t MaxQueues = 32;
// docs/dlssnr/design/chain-in-flight.md. Ticks are the renderer's NR entry count (Model::Advance).
// How long an epoch whose fences cannot answer -- never submitted, a failed Signal -- holds up ADMISSION, and how
// long a completed but unsealed epoch stays in a lifetime set so a replay is still caught. Never destruction
// evidence: retirement needs Finished.
constexpr uint64_t AgeOutTicks = 128;
// How far back the oldest recording the GPU may still read can be, in NR entries, before nothing new is
// admitted. Every entry can take up to six slots of the main pass's 64-slot descriptor/constant ring (and one to
// seven of the sub-pass rings, 16 to 112 slots); eight entries cannot wrap any of them over a recording that has
// not finished, whether it is still waiting to be submitted or running behind on the GPU.
constexpr uint64_t RingTicks = 8;
// Tracked batches seen on one queue before recordings may overlap. Until then, and for good after tracked work is
// seen on a second queue, a recording waits for the previous one to be done: nothing is injected into the game's
// queues to order them, because only the game knows its own cross-queue dependencies.
constexpr uint64_t AffinityBatches = 8;
// Earlier tracked recordings that may still be unresolved when a new one starts. With the new one, at most
// four NR recordings are on the GPU: 24 of the main pass's 64 ring slots at six dispatches each.
constexpr unsigned MaxEarlierInFlight = 3;

struct Epoch
{
    uintptr_t list = 0;
    bool sealed = false;
    bool submitted = false;
    bool unknown = false;
    unsigned pending = 0;
    uint64_t executions = 0; // Every observed list occurrence, including duplicates in one batch.
    uint64_t trackedAt = 0;  // Model clock when last tracked.
    // Only our reference to the list is left (the game released it), refreshed by the caller: nobody can
    // execute it again.
    bool orphaned = false;
    // A recording of the NR chain (TrackNrRecording), as opposed to the module's other tracked work: only these
    // establish or break the one-queue affinity that lets recordings overlap.
    bool chain = false;
    std::array<uint64_t, MaxQueues> fences {};
    // Keeps COM identity alive to prevent pointer reuse; absent in host tests.
    std::shared_ptr<void> owner;
};

struct Usage
{
    std::array<std::shared_ptr<Epoch>, MaxUses> epochs {};
};

template <typename Completed> bool IsComplete(const Epoch& e, Completed completed)
{
    if (!e.submitted || e.unknown || e.pending != 0)
        return false;
    for (size_t q = 0; q < MaxQueues; ++q)
    {
        if (e.fences[q] == 0)
            continue;
        const auto value = completed(q);
        if (value == std::numeric_limits<uint64_t>::max() || value < e.fences[q])
            return false;
    }
    return true;
}

template <typename Completed> bool IsReady(const Epoch& e, Completed completed)
{
    // Even a completed, closed list can execute again. Only successful Reset seals it.
    return e.sealed && IsComplete(e, completed);
}

// Recorded, then discarded by a successful Reset before any execution was seen: those commands can never run.
inline bool IsCancelled(const Epoch& e)
{
    return e.sealed && !e.submitted && !e.unknown && e.pending == 0 && e.executions == 0;
}

inline bool IsAged(const Epoch& e, uint64_t clock)
{
    return clock >= e.trackedAt && clock - e.trackedAt >= AgeOutTicks;
}

// Safe in any mode: the GPU is done with it and nothing can execute it again. Ready (Reset after completion);
// cancelled; or a list the game released, so no one holds it, that either completed or never executed.
template <typename Completed> bool IsSafe(const Epoch& e, Completed completed)
{
    if (IsReady(e, completed) || IsCancelled(e))
        return true;
    return e.orphaned && e.pending == 0 && !e.unknown &&
           (IsComplete(e, completed) || (!e.submitted && e.executions == 0));
}

// Done on the GPU, as far as starting new work goes. Outside strict mode a completed epoch counts although its
// list could replay -- the trust Streamline, ReShade and NGX place in the host.
template <typename Completed> bool IsAdmissionDone(const Epoch& e, Completed completed, bool strict)
{
    return IsSafe(e, completed) || (!strict && IsComplete(e, completed));
}

// Done for FREEING resources. A completed, unsealed epoch counts only once it aged out of the replay window, so a
// replay inside the window (which latches strict) still finds what it uses alive. Ticks never stand in for a
// fence: anything unknown, unsubmitted or outstanding stays unfinished.
template <typename Completed> bool IsFinished(const Epoch& e, Completed completed, uint64_t clock, bool strict)
{
    return IsSafe(e, completed) || (!strict && IsComplete(e, completed) && IsAged(e, clock));
}

// No fence will ever answer for it: an execution whose Signal failed, or a recording never submitted that the
// game still holds.
inline bool IsUnanswerable(const Epoch& e)
{
    return e.pending == 0 && (e.unknown || (!e.submitted && e.executions == 0));
}

// Every epoch finished: what retirement waits for.
template <typename Completed> bool IsSettled(const Usage& usage, Completed completed, uint64_t clock, bool strict)
{
    for (const auto& e : usage.epochs)
        if (e && !IsFinished(*e, completed, clock, strict))
            return false;
    return true;
}

// Not settled, but only because of epochs no fence will answer that aged out: a parked object waiting on them is
// retained (never freed without evidence) and no longer counts toward the parking cap. Recomputed every tick.
template <typename Completed>
bool IsOnlyUnanswerableLeft(const Usage& usage, Completed completed, uint64_t clock, bool strict)
{
    bool any = false;
    for (const auto& e : usage.epochs)
    {
        if (!e || IsFinished(*e, completed, clock, strict))
            continue;
        if (!IsUnanswerable(*e) || !IsAged(*e, clock))
            return false;
        any = true;
    }
    return any;
}

// Drops only the epochs IsSettled no longer waits for, one by one: never one that is merely admission-done, whatever
// the usage's free space. For a usage that protects a resource's lifetime (the exposure scan's candidates): a full
// usage then refuses new recordings rather than forgetting a list that can still execute again.
template <typename Completed> void PruneFinished(Usage& usage, Completed completed, uint64_t clock, bool strict)
{
    for (auto& e : usage.epochs)
        if (e && IsFinished(*e, completed, clock, strict))
            e.reset();
}

// The lifetime set keeps what a retirement snapshot or a replay could still need. Drops what is finished; then,
// outside strict mode and only when fewer than MinFreeUses slots are left, the oldest admission-done epochs (their
// replay window is cut short, rather than refusing every later recording for want of a slot).
constexpr size_t MinFreeUses = 8;
template <typename Completed> void PruneLifetime(Usage& usage, Completed completed, uint64_t clock, bool strict)
{
    size_t free = 0;
    for (auto& e : usage.epochs)
    {
        if (e && IsFinished(*e, completed, clock, strict))
            e.reset();
        free += !e;
    }
    while (!strict && free < MinFreeUses)
    {
        std::shared_ptr<Epoch>* oldest = nullptr;
        for (auto& e : usage.epochs)
            if (e && IsAdmissionDone(*e, completed, false) && (!oldest || e->trackedAt < (*oldest)->trackedAt))
                oldest = &e;
        if (!oldest)
            break;
        oldest->reset();
        ++free;
    }
}

enum class Admission
{
    Admit,
    InFlight, // more than MaxEarlierInFlight earlier recordings unfinished
    Queues,   // unfinished work on more than one queue, or the session already switched queues
    SameList, // the list already holds an unsubmitted NR recording: a second one would reuse its ring slots
    Pending,  // a recording has waited RingTicks for its first submission: new work would wrap the rings over it
    Behind,   // a submitted recording is RingTicks old and not done: the GPU is that far behind
    Serial,   // one queue is not established (or more than one was seen): no overlap
    Replay,   // strict mode, and something is not Ready
};

// May a new tracked recording into list start? Does not modify usage. overlap: the model has established that
// tracked work runs on one queue (Model::Overlap).
template <typename Completed>
Admission Admit(const Usage& usage, Completed completed, uint64_t clock, bool strict, bool overlap, uintptr_t list,
                unsigned maxEarlier = MaxEarlierInFlight)
{
    unsigned open = 0;
    size_t queue = MaxQueues;
    bool mixed = false;
    for (const auto& e : usage.epochs)
    {
        if (!e || IsAdmissionDone(*e, completed, strict))
            continue;
        if (strict)
            return Admission::Replay;
        const bool unsubmitted = !e->submitted && !e->unknown && e->executions == 0;
        if (unsubmitted && e->list == list && !e->sealed)
            return Admission::SameList;
        // An epoch whose Signal failed keeps protecting its ring slots: nothing says it has run, so it ends in Behind
        // like any outstanding recording (in practice the device is gone when a Signal fails).
        if (clock >= e->trackedAt && clock - e->trackedAt >= RingTicks)
            return unsubmitted ? Admission::Pending : Admission::Behind;
        if (!overlap)
            return Admission::Serial;
        ++open;
        for (size_t q = 0; q < MaxQueues; ++q)
        {
            if (e->fences[q] == 0)
                continue;
            if (queue == MaxQueues)
                queue = q;
            else if (queue != q)
                mixed = true;
        }
    }
    if (mixed)
        return Admission::Queues;
    return open > maxEarlier ? Admission::InFlight : Admission::Admit;
}

inline bool IsSubmittedOnlyOn(const Epoch& e, size_t targetQueue)
{
    if (!e.submitted || e.unknown || e.pending != 0)
        return false;
    bool foundTarget = false;
    for (size_t q = 0; q < MaxQueues; ++q)
    {
        if (e.fences[q] != 0)
        {
            if (q != targetQueue)
                return false;
            foundTarget = true;
        }
    }
    return foundTarget;
}

class Model
{
    std::array<std::shared_ptr<Epoch>, MaxEpochs> epochs {};
    uint64_t clock = 0;
    bool replayed = false;
    size_t affinity = MaxQueues; // the queue tracked work was last submitted on
    uint64_t affinityBatches = 0;
    bool multiQueue = false;

    // A slot can be reused for a new epoch. Besides Ready: a cancelled epoch, and outside strict mode a completed
    // one nobody holds that has not been tracked for AgeOutTicks. Its list was never Reset again; without this a
    // game that abandons lists would fill the tracker and stop every tracked recording.
    template <typename Completed> bool Reclaimable(const std::shared_ptr<Epoch>& e, Completed completed) const
    {
        if (IsReady(*e, completed))
            return true;
        if (e.use_count() != 1)
            return false;
        return IsSafe(*e, completed) || (!replayed && IsComplete(*e, completed) && IsAged(*e, clock));
    }

    void NoteOccurrence(const std::shared_ptr<Epoch>& e)
    {
        CountExecution(e);
        if (e->executions > 1)
            replayed = true;
    }

  public:
    // Monotonic; the renderer passes its evaluation count.
    void Advance(uint64_t now)
    {
        if (now > clock)
            clock = now;
    }
    uint64_t Clock() const { return clock; }
    // A list executed again without a Reset: replay is real in this process.
    bool Strict() const { return replayed; }
    // Tracked work went to more than one queue (or one the tracker could not admit) this session.
    bool MultiQueue() const { return multiQueue; }
    // Recordings may overlap: AffinityBatches tracked batches all on one queue, and never a second queue.
    bool Overlap() const { return !multiQueue && affinityBatches >= AffinityBatches; }

    // Before tracked work executes on queue (MaxQueues: a queue the tracker could not admit). Returns the queue it
    // previously went to when this is a switch (MaxQueues otherwise); a switch latches MultiQueue for the session.
    size_t NoteQueue(size_t queue)
    {
        if (queue >= MaxQueues)
        {
            multiQueue = true;
            return affinity;
        }
        if (affinity == MaxQueues || affinity == queue)
        {
            affinity = queue;
            ++affinityBatches;
            return MaxQueues;
        }
        const size_t previous = affinity;
        affinity = queue;
        multiQueue = true;
        return previous;
    }

    std::shared_ptr<Epoch> Current(uintptr_t list) const
    {
        for (const auto& e : epochs)
            if (e && e->list == list && !e->sealed)
                return e;
        return {};
    }

    template <typename Completed> std::shared_ptr<Epoch> Track(uintptr_t list, Usage& usage, Completed completed)
    {
        size_t freeUse = MaxUses;
        auto current = Current(list);
        if (list == 0 || (current && current->unknown))
            return {};
        for (size_t i = 0; i < MaxUses; ++i)
        {
            auto& e = usage.epochs[i];
            if (e && IsReady(*e, completed))
                e.reset();
            if (current && e == current)
            {
                current->trackedAt = clock;
                return current;
            }
            if (!e)
                freeUse = i;
        }
        if (freeUse == MaxUses)
            return {};
        if (!current)
        {
            for (auto& e : epochs)
            {
                if (e && Reclaimable(e, completed))
                    e.reset();
                if (!e)
                {
                    e = std::make_shared<Epoch>();
                    e->list = list;
                    current = e;
                    break;
                }
            }
        }
        if (current)
        {
            current->trackedAt = clock;
            usage.epochs[freeUse] = current;
        }
        return current;
    }

    static void CountExecution(const std::shared_ptr<Epoch>& e)
    {
        if (!e)
            return;
        if (e->executions == std::numeric_limits<uint64_t>::max())
            e->unknown = true;
        else
            ++e->executions;
    }

    void Abandon(uintptr_t list)
    {
        for (auto& e : epochs)
        {
            if (e && e->list == list && !e->submitted && e->executions == 0)
            {
                e.reset();
            }
        }
    }

    std::shared_ptr<Epoch> BeforeExecute(uintptr_t list)
    {
        auto e = Current(list);
        if (e)
        {
            ++e->pending;
            NoteOccurrence(e);
        }
        return e;
    }

    // The same recording again within one ExecuteCommandLists batch: another occurrence, no second notification.
    void DuplicateInBatch(const std::shared_ptr<Epoch>& e)
    {
        if (e)
            NoteOccurrence(e);
    }

    static void AfterExecute(const std::shared_ptr<Epoch>& e, size_t queue, uint64_t fence, bool success)
    {
        if (!e)
            return;
        if (!success || queue >= MaxQueues || fence == 0 || fence == std::numeric_limits<uint64_t>::max())
            e->unknown = true;
        else
        {
            e->submitted = true;
            if (e->fences[queue] < fence)
                e->fences[queue] = fence;
        }
        if (e->pending != 0)
            --e->pending;
    }

    static void AfterReset(const std::shared_ptr<Epoch>& e, bool success)
    {
        if (e && success)
            e->sealed = true;
    }
};
} // namespace DlssNr::Submission::Detail
