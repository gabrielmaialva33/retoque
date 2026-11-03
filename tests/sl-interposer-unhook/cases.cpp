// retoque/hooks/DetourRecord.h: which detours a hook set really attached, and an unhook that detaches exactly those.
// Detours is a fake that answers like the real one: an attach can be refused, a detach of a pointer that was never
// attached poisons the transaction, and a commit applies all or nothing.
#include "retoque/hooks/DetourRecord.h"

#include <cassert>
#include <cstdio>
#include <set>
#include <utility>
#include <vector>

using Retoque::Hooks::DetourRecord;

namespace
{
// Detours as documented (4.0.1, detours.cpp): an attach can be refused at once without harming the transaction
// (a bad argument), or fail in a way that poisons it (an allocation failure records a pending error and the commit
// aborts the whole batch); a detach of a pointer holding no trampoline poisons it too. A commit applies all or
// nothing: on attach the original pointer becomes the trampoline, on detach it gets the real function back.
struct FakeDetours
{
    struct Op
    {
        void** target;
        void* detour;
        bool attach;
    };
    std::set<void**> live;      // attached after the last commit
    std::vector<Op> ops;        // this transaction's requests, in order
    std::set<void**> refuse;    // attaches refused at once (ERROR_INVALID_PARAMETER), transaction unharmed
    std::set<void**> exhausted; // attaches that fail to allocate: the transaction is poisoned
    bool failCommit = false;
    bool poisoned = false;
    int trampolines = 0;

    void Begin()
    {
        ops.clear();
        poisoned = false;
    }
    long Attach(void** target, void* detour)
    {
        if (refuse.count(target) != 0)
            return 87;
        if (exhausted.count(target) != 0)
        {
            poisoned = true;
            return 8;
        }
        ops.push_back({ target, detour, true });
        return 0;
    }
    long Detach(void** target, void* detour)
    {
        if (live.count(target) == 0)
        {
            poisoned = true; // the trampoline check rejects it
            return 13;
        }
        ops.push_back({ target, detour, false });
        return 0;
    }
    bool Commit()
    {
        if (failCommit || poisoned)
            return false;
        for (const auto& op : ops)
        {
            if (op.attach)
            {
                live.insert(op.target);
                *op.target = Trampoline(op.target);
            }
            else
            {
                live.erase(op.target);
                *op.target = Real(op.target);
            }
        }
        return true;
    }
    static void* Trampoline(void** target) { return reinterpret_cast<char*>(target) + 0x7000; }
    static void* Real(void** target);
};

int g_realA, g_realB, g_realC;
void* oA = &g_realA; // stand-ins for o_slInit and friends, holding the real function until hooked
void* oB = &g_realB;
void* oC = &g_realC;
void* FakeDetours::Real(void** target) { return target == &oA ? &g_realA : target == &oB ? &g_realB : &g_realC; }
void* const kDetour1 = reinterpret_cast<void*>(0x1001);
void* const kDetour2 = reinterpret_cast<void*>(0x1002);
void* const kDetour3 = reinterpret_cast<void*>(0x1003);

void ResetOriginals()
{
    oA = &g_realA;
    oB = &g_realB;
    oC = &g_realC;
}

void Hook(DetourRecord& record, FakeDetours& detours, std::vector<std::pair<void**, void*>> wanted)
{
    detours.Begin();
    record.Begin();
    for (const auto& [target, detour] : wanted)
        record.Attach([&](void** t, void* d) { return detours.Attach(t, d); }, target, detour);
    record.Committed(detours.Commit());
}

bool Unhook(DetourRecord& record, FakeDetours& detours)
{
    detours.Begin();
    record.DetachAll([&](void** t, void* d) { return detours.Detach(t, d); });
    const bool ok = detours.Commit();
    record.Detached(ok);
    return ok;
}

void OnlyWhatWasAttachedComesOff()
{
    // The bug: the unhook used to detach every export it had looked up. oC was looked up but never attached (no
    // Streamline FG input); detaching it poisoned the transaction and nothing came off.
    ResetOriginals();
    DetourRecord record;
    FakeDetours detours;
    Hook(record, detours, { { &oA, kDetour1 }, { &oB, kDetour2 } });
    assert(oA == FakeDetours::Trampoline(&oA) && oB == FakeDetours::Trampoline(&oB));
    assert(record.Size() == 2 && record.Attached(&oA) && record.Attached(&oB) && !record.Attached(&oC));
    assert(Unhook(record, detours));
    assert(detours.live.empty() && record.Size() == 0);
    assert(oA == nullptr && oB == nullptr && oC == &g_realC); // cleared only what it attached
}

void NewestFirst()
{
    ResetOriginals();
    DetourRecord record;
    FakeDetours detours;
    Hook(record, detours, { { &oA, kDetour1 }, { &oB, kDetour2 }, { &oC, kDetour3 } });
    Unhook(record, detours);
    assert(detours.ops.size() == 3);
    assert(detours.ops[0].target == &oC && detours.ops[1].target == &oB && detours.ops[2].target == &oA);
    assert(detours.ops[0].detour == kDetour3 && detours.ops[2].detour == kDetour1);
}

void RefusedAttachIsNotRecorded()
{
    // Refused at once: the rest of the transaction commits, and the refused one is not detached later.
    ResetOriginals();
    DetourRecord record;
    FakeDetours detours;
    detours.refuse.insert(&oB);
    Hook(record, detours, { { &oA, kDetour1 }, { &oB, kDetour2 } });
    assert(record.Size() == 1 && record.Attached(&oA) && !record.Attached(&oB) && oB == &g_realB);
    assert(Unhook(record, detours)); // oB is not detached, so the transaction is not poisoned
}

void PoisonedAttachRecordsNothing()
{
    // An allocation failure poisons the transaction: the commit aborts the whole batch, nothing is recorded and no
    // original becomes a trampoline.
    ResetOriginals();
    DetourRecord record;
    FakeDetours detours;
    detours.exhausted.insert(&oB);
    Hook(record, detours, { { &oA, kDetour1 }, { &oB, kDetour2 } });
    assert(record.Size() == 0 && detours.live.empty() && oA == &g_realA && oB == &g_realB);
    assert(Unhook(record, detours));
}

void FailedCommitRecordsNothing()
{
    ResetOriginals();
    DetourRecord record;
    FakeDetours detours;
    detours.failCommit = true;
    Hook(record, detours, { { &oA, kDetour1 } });
    assert(record.Size() == 0 && detours.live.empty() && oA == &g_realA);
    detours.failCommit = false;
    assert(Unhook(record, detours)); // nothing to take off
    assert(oA == &g_realA);
}

void FailedUnhookKeepsTheRecord()
{
    ResetOriginals();
    DetourRecord record;
    FakeDetours detours;
    Hook(record, detours, { { &oA, kDetour1 }, { &oB, kDetour2 } });
    detours.failCommit = true;
    assert(!Unhook(record, detours));
    // Still attached: the record stays, and the originals still hold the trampolines the live hooks call.
    assert(record.Size() == 2 && oA == FakeDetours::Trampoline(&oA) && oB == FakeDetours::Trampoline(&oB));
    detours.failCommit = false;
    assert(Unhook(record, detours) && record.Size() == 0 && oA == nullptr && oB == nullptr);
}

void TwoTransactionsAccumulate()
{
    // Streamline 1 and 2 hook in separate transactions; the unhook takes both off.
    ResetOriginals();
    DetourRecord record;
    FakeDetours detours;
    Hook(record, detours, { { &oA, kDetour1 } });
    Hook(record, detours, { { &oB, kDetour2 } });
    assert(record.Size() == 2);
    assert(Unhook(record, detours) && detours.live.empty());
}

void ABeginDropsAnUncommittedTransaction()
{
    ResetOriginals();
    DetourRecord record;
    FakeDetours detours;
    record.Begin();
    record.Attach([&](void** t, void* d) { return detours.Attach(t, d); }, &oA, kDetour1);
    record.Begin(); // that transaction was never committed
    record.Committed(true);
    assert(record.Size() == 0);
}
} // namespace

int main()
{
    OnlyWhatWasAttachedComesOff();
    NewestFirst();
    RefusedAttachIsNotRecorded();
    PoisonedAttachRecordsNothing();
    FailedCommitRecordsNothing();
    FailedUnhookKeepsTheRecord();
    TwoTransactionsAccumulate();
    ABeginDropsAnUncommittedTransaction();
    std::puts("sl-interposer-unhook: 8 cases passed");
    return 0;
}
