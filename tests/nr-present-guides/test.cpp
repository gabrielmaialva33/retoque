#include "pch.h"
#include <dlssnr/DlssNr_PresentGuideRing.h>
#include <dlssnr/DlssNr_SubmissionModel.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <unordered_set>

using namespace DlssNr::Submission::Detail;
using namespace DlssNr::PresentGuides;

// This is the same model predicate called by Submission::CaptureReusable in production.
template <typename Done> static bool CaptureBusy(void* token, const Done& done)
{
    return token && !IsCaptureReusable(*static_cast<Usage*>(token), [&](size_t q) { return done[q]; });
}

void TestCaptureReuseAndCancelledRing()
{
    Model model;
    std::array<uint64_t, MaxQueues> done {};
    auto completed = [&](size_t q) { return done[q]; };
    Usage u;
    auto epoch = model.Track(101, u, completed);
    assert(epoch && CaptureBusy(&u, done)); // unsubmitted, still executable
    auto pending = model.BeforeExecute(101);
    assert(CaptureBusy(&u, done));
    Model::AfterExecute(pending, 0, 1, true);
    assert(CaptureBusy(&u, done)); // outstanding write
    done[0] = 1;
    assert(!CaptureBusy(&u, done)); // completed, no Reset required for overwrite admission
    done[0] = UINT64_MAX;
    assert(CaptureBusy(&u, done)); // device loss is not completion
    done[0] = 1;
    Usage failed;
    auto failure = model.Track(102, failed, completed);
    assert(failure);
    auto signal = model.BeforeExecute(102);
    Model::AfterExecute(signal, 0, 2, false);
    Model::AfterReset(model.Current(102), true);
    assert(CaptureBusy(&failed, done)); // failed signal, even after Reset
    failure->orphaned = true;
    assert(CaptureBusy(&failed, done));
    Usage orphan;
    auto o = model.Track(103, orphan, completed);
    o->orphaned = true;
    assert(!CaptureBusy(&orphan, done)); // definitively released without any Execute
    o->unknown = true;
    assert(CaptureBusy(&orphan, done));
    Usage empty;
    assert(CaptureBusy(&empty, done));
    Usage multiple = u;
    multiple.epochs[1] = epoch;
    assert(CaptureBusy(&multiple, done)); // enforce the single-epoch capture contract

    Ring ring;
    std::array<Usage, kRingSize> captures {};
    auto noRead = [](void*) { return false; };
    auto writeBusy = [&](void* token) { return CaptureBusy(token, done); };
    auto shape = [](size_t) { return true; };
    for (size_t i = 0; i < kRingSize; ++i)
    {
        assert(ring.AcquireForRecord(noRead, writeBusy, shape) == i);
        assert(model.Track(200 + i, captures[i], completed));
        ring.CommitRecord(i, 1920, 1080, false, 1000 + i, &captures[i], nullptr);
    }
    assert(ring.AcquireForRecord(noRead, writeBusy, shape) == kRingSize);
    for (size_t i = 0; i < kRingSize; ++i)
        Model::AfterReset(model.Current(200 + i), true); // discard all captures without Execute
    for (size_t i = 0; i < kRingSize; ++i)
    {
        const size_t slot = ring.AcquireForRecord(noRead, writeBusy, shape);
        assert(slot == i); // no permanent four-slot starvation after successful Reset
        captures[slot] = {};
        assert(model.Track(300 + i, captures[slot], completed));
        ring.CommitRecord(slot, 1920, 1080, false, 2000 + i, &captures[slot], nullptr);
    }
}

void TestRingBasicAcquireCommitPick()
{
    std::unordered_set<void*> completedWrites;
    std::unordered_set<void*> inFlightReads;

    auto isWriteCompleted = [&](void* token) { return completedWrites.find(token) != completedWrites.end(); };
    auto isReadInFlight = [&](void* token) { return inFlightReads.find(token) != inFlightReads.end(); };

    Ring ring;

    // Initially no slots recorded
    assert(ring.PickForPresent(isWriteCompleted, 1000) == kRingSize);

    // Slot 0 recorded with writeToken 0x1, readToken 0x101
    void* wToken1 = (void*) 0x1;
    void* rToken1 = (void*) 0x101;
    size_t s0 = ring.AcquireForRecord(isReadInFlight);
    assert(s0 == 0);
    ring.CommitRecord(s0, 1920, 1080, true, 1000, wToken1, rToken1);

    // Not yet completed by submission tracker: pick returns kRingSize (falls back to zero guides, no stall)
    assert(ring.PickForPresent(isWriteCompleted, 1010) == kRingSize);

    // Submission completes on GPU
    completedWrites.insert(wToken1);
    size_t picked = ring.PickForPresent(isWriteCompleted, 1015);
    assert(picked == 0);
    assert(ring.GetSlot(picked).sequence == 1);
    assert(ring.GetSlot(picked).width == 1920);

    // Mark slot 0 read as in-flight
    inFlightReads.insert(rToken1);

    // Next record: must not overwrite slot 0 (it is both last-read and in-flight)
    void* wToken2 = (void*) 0x2;
    void* rToken2 = (void*) 0x102;
    size_t s1 = ring.AcquireForRecord(isReadInFlight);
    assert(s1 != 0); // picks a different slot
    ring.CommitRecord(s1, 1920, 1080, true, 1020, wToken2, rToken2);

    // wToken2 not completed yet; present continues reading newest completed (slot 0)
    assert(ring.PickForPresent(isWriteCompleted, 1025) == 0);

    // wToken2 completes
    completedWrites.insert(wToken2);
    size_t picked2 = ring.PickForPresent(isWriteCompleted, 1030);
    assert(picked2 == s1);
    assert(ring.GetSlot(picked2).sequence == 2);
}

void TestRingInFlightReadProtectionNotLastRead()
{
    // Test: a slot currently in-flight that is NOT the last-read index must NOT be overwritten!
    std::unordered_set<void*> completedWrites;
    std::unordered_set<void*> inFlightReads;

    auto isWriteCompleted = [&](void* token) { return completedWrites.find(token) != completedWrites.end(); };
    auto isReadInFlight = [&](void* token) { return inFlightReads.find(token) != inFlightReads.end(); };

    Ring ring;

    void* w[4] = { (void*) 0x1, (void*) 0x2, (void*) 0x3, (void*) 0x4 };
    void* r[4] = { (void*) 0x11, (void*) 0x12, (void*) 0x13, (void*) 0x14 };

    // Record and complete slot 0
    size_t s0 = ring.AcquireForRecord(isReadInFlight);
    ring.CommitRecord(s0, 1920, 1080, false, 1000, w[0], r[0]);
    completedWrites.insert(w[0]);

    // Present reads slot 0
    assert(ring.PickForPresent(isWriteCompleted, 1010) == s0);
    inFlightReads.insert(r[0]); // slot 0 read is in flight

    // Record and complete slot 1
    size_t s1 = ring.AcquireForRecord(isReadInFlight);
    ring.CommitRecord(s1, 1920, 1080, false, 1020, w[1], r[1]);
    completedWrites.insert(w[1]);

    // Present reads slot 1 -> lastReadIndex becomes s1 (1)
    assert(ring.PickForPresent(isWriteCompleted, 1030) == s1);
    assert(ring.LastReadIndex() == s1);

    // Now slot 0 read is STILL in flight (r[0] in inFlightReads), but lastReadIndex is s1!
    // Next records must NOT touch slot 0 even though it is not lastReadIndex!
    size_t s2 = ring.AcquireForRecord(isReadInFlight);
    assert(s2 != s0 && s2 != s1);
    ring.CommitRecord(s2, 1920, 1080, false, 1040, w[2], r[2]);

    size_t s3 = ring.AcquireForRecord(isReadInFlight);
    assert(s3 != s0 && s3 != s1 && s3 != s2);
    ring.CommitRecord(s3, 1920, 1080, false, 1050, w[3], r[3]);

    // Slot 0 is still in flight (r[0] in inFlightReads). Slot 1 is lastReadIndex.
    // Calling AcquireForRecord now: slot 0 MUST NOT be chosen because its read is in flight!
    size_t nextAcquire = ring.AcquireForRecord(isReadInFlight);
    assert(nextAcquire != s0);
    assert(nextAcquire != s1);
}

void TestRingAllSlotsInFlightSkipsCapture()
{
    // Test: when all non-last-read slots are currently in-flight on GPU,
    // AcquireForRecord must return kRingSize (skip capture, matching RenoDX).
    std::unordered_set<void*> completedWrites;
    std::unordered_set<void*> inFlightReads;

    auto isWriteCompleted = [&](void* token) { return completedWrites.find(token) != completedWrites.end(); };
    auto isReadInFlight = [&](void* token) { return inFlightReads.find(token) != inFlightReads.end(); };

    Ring ring;

    void* w[4] = { (void*) 0x1, (void*) 0x2, (void*) 0x3, (void*) 0x4 };
    void* r[4] = { (void*) 0x11, (void*) 0x12, (void*) 0x13, (void*) 0x14 };

    // Record all 4 slots and mark all of them as in-flight reads
    for (int i = 0; i < 4; ++i)
    {
        size_t s = ring.AcquireForRecord(isReadInFlight);
        ring.CommitRecord(s, 1920, 1080, false, 1000 + i * 10, w[i], r[i]);
        completedWrites.insert(w[i]);
    }

    // Set slot 3 as lastReadIndex, and mark slots 0, 1, 2 in flight
    ring.PickForPresent(isWriteCompleted, 1040); // picks 3
    assert(ring.LastReadIndex() == 3);

    inFlightReads.insert(r[0]);
    inFlightReads.insert(r[1]);
    inFlightReads.insert(r[2]);

    // All slots are either lastReadIndex (3) or inFlightReads (0, 1, 2)
    // AcquireForRecord MUST return kRingSize (skip capture)
    assert(ring.AcquireForRecord(isReadInFlight) == kRingSize);

    // When read on slot 1 completes:
    inFlightReads.erase(r[1]);
    assert(ring.AcquireForRecord(isReadInFlight) == 1);
}

void TestRingMaxAgeStaleRejection()
{
    // Test: captures older than kPresentGuideMaxAgeMs (250 ms) are dropped in favor of neutral guides.
    // Slot 0 is protected as lastReadIndex so it cannot be overwritten while more frames are recorded.
    std::unordered_set<void*> completedWrites;
    std::unordered_set<void*> inFlightReads;

    auto isWriteCompleted = [&](void* token) { return completedWrites.find(token) != completedWrites.end(); };
    auto isReadInFlight = [&](void* token) { return inFlightReads.find(token) != inFlightReads.end(); };

    Ring ring;

    void* w0 = (void*) 0x10;
    void* r0 = (void*) 0x20;

    // Record slot 0 at nowMs = 1000, and complete it
    size_t s0 = ring.AcquireForRecord(isReadInFlight);
    assert(s0 == 0);
    ring.CommitRecord(s0, 1920, 1080, false, 1000, w0, r0);
    completedWrites.insert(w0);

    // Present reads slot 0 at 1010 ms -> slot 0 is now lastReadIndex (protected against overwrite)
    assert(ring.PickForPresent(isWriteCompleted, 1010) == s0);
    assert(ring.LastReadIndex() == s0);

    // Record 3 more frames (slots 1, 2, 3) at 1020, 1030, 1040 ms without completing their writes
    for (int i = 1; i <= 3; ++i)
    {
        void* w = (void*) (uintptr_t) (0x10 + i);
        void* r = (void*) (uintptr_t) (0x20 + i);
        size_t s = ring.AcquireForRecord(isReadInFlight);
        assert(s != s0);
        ring.CommitRecord(s, 1920, 1080, false, 1010 + i * 10, w, r);
    }

    // At 1100 ms (elapsed 100 ms <= 250 ms), slot 0 is still fresh and picked
    assert(ring.PickForPresent(isWriteCompleted, 1100) == s0);

    // At 1300 ms (elapsed 300 ms > 250 ms), slot 0 is stale!
    // PickForPresent must reject slot 0 as stale and return kRingSize
    assert(ring.PickForPresent(isWriteCompleted, 1300) == kRingSize);
}

void TestProductionSignature()
{
    // Item 1b: Verify exact production signature compiles with lambdas capturing state
    Ring ring;
    void* queue = (void*) 0x1234;
    const auto isEligible = [queue](void* token) -> bool { return token != nullptr && token == queue; };
    const auto isReadInFlight = [queue](void* token) -> bool { return token != nullptr && token != queue; };
    const auto isWriteInFlight = [queue](void* token) -> bool { return token != nullptr && token != queue; };
    const auto isShapeMatch = [](size_t) -> bool { return false; };
    AcquireReport report {};
    assert(ring.AcquireForRecord(isReadInFlight, isWriteInFlight, isShapeMatch, &report) == 0);
    assert(report.slot == 0 && report.kind == AcquireKind::ReusedEmpty);
    assert(ring.PickForPresent(isEligible, 0) == kRingSize);
}

void TestRingInFlightWriteProtectionAndBusyReport()
{
    std::unordered_set<void*> completedWrites;
    std::unordered_set<void*> inFlightReads;
    std::unordered_set<void*> inFlightWrites;

    auto isWriteCompleted = [&](void* token) { return completedWrites.find(token) != completedWrites.end(); };
    auto isReadInFlight = [&](void* token) { return inFlightReads.find(token) != inFlightReads.end(); };
    auto isWriteInFlight = [&](void* token) { return inFlightWrites.find(token) != inFlightWrites.end(); };
    auto noShapeMatch = [](size_t) { return false; };

    Ring ring;
    void* w[4] = { (void*) 0x1, (void*) 0x2, (void*) 0x3, (void*) 0x4 };
    void* r[4] = { (void*) 0x11, (void*) 0x12, (void*) 0x13, (void*) 0x14 };

    for (int i = 0; i < 4; ++i)
    {
        AcquireReport rep {};
        size_t s = ring.AcquireForRecord(isReadInFlight, isWriteInFlight, noShapeMatch, &rep);
        assert(s == static_cast<size_t>(i));
        assert(rep.kind == AcquireKind::ReusedEmpty);
        ring.CommitRecord(s, 1920, 1080, false, 1000 + i * 10, w[i], r[i]);
    }

    // Complete slot 3's write and pick it at Present so slot 3 is lastReadIndex.
    completedWrites.insert(w[3]);
    assert(ring.PickForPresent(isWriteCompleted, 1040) == 3);
    assert(ring.LastReadIndex() == 3);

    // Slot 0: read in flight. Slots 1 and 2: capture writes still in flight.
    inFlightReads.insert(r[0]);
    inFlightWrites.insert(w[1]);
    inFlightWrites.insert(w[2]);

    AcquireReport busy {};
    assert(ring.AcquireForRecord(isReadInFlight, isWriteInFlight, noShapeMatch, &busy) == kRingSize);
    assert(busy.slot == kRingSize);
    assert(busy.kind == AcquireKind::None);
    assert(busy.busyLastRead == 1);
    assert(busy.busyReadInFlight == 1);
    assert(busy.busyWriteInFlight == 2);

    // Once slot 2's capture write completes on the GPU, only slot 2 is eligible.
    inFlightWrites.erase(w[2]);
    AcquireReport freed {};
    assert(ring.AcquireForRecord(isReadInFlight, isWriteInFlight, noShapeMatch, &freed) == 2);
    assert(freed.slot == 2);
    assert(freed.kind == AcquireKind::EvictedOldest);
}

void TestRingShapePreferenceOrder()
{
    auto noReadInFlight = [](void*) { return false; };
    auto noWriteInFlight = [](void*) { return false; };
    auto allWritesDone = [](void*) { return true; };

    Ring ring;
    void* w[4] = { (void*) 0x1, (void*) 0x2, (void*) 0x3, (void*) 0x4 };
    void* r[4] = { (void*) 0x11, (void*) 0x12, (void*) 0x13, (void*) 0x14 };

    // Populate slots 0, 2, 3; leave slot 1 empty by invalidating it after initial setup.
    ring.CommitRecord(0, 1920, 1080, false, 1000, w[0], r[0]); // seq 1 (mismatched)
    ring.CommitRecord(1, 1920, 1080, false, 1010, w[1], r[1]); // seq 2 -> will invalidate
    ring.CommitRecord(2, 2560, 1440, false, 1020, w[2], r[2]); // seq 3 (matching)
    ring.CommitRecord(3, 2560, 1440, false, 1030, w[3], r[3]); // seq 4 -> will become lastReadIndex
    ring.InvalidateSlot(1);
    assert(ring.PickForPresent(allWritesDone, 1040) == 3);
    assert(ring.LastReadIndex() == 3);

    // 1. Prefer free matching slot (slot 2, seq 3) over empty slot (slot 1) and older mismatched slot (slot 0, seq 1).
    AcquireReport repMatch {};
    size_t pickedMatch =
        ring.AcquireForRecord(noReadInFlight, noWriteInFlight, [](size_t i) { return i == 2; }, &repMatch);
    assert(pickedMatch == 2);
    assert(repMatch.kind == AcquireKind::ReusedMatching);

    // 2. When no free slot matches, prefer empty slot (slot 1) over older mismatched slot (slot 0, seq 1).
    AcquireReport repEmpty {};
    size_t pickedEmpty =
        ring.AcquireForRecord(noReadInFlight, noWriteInFlight, [](size_t) { return false; }, &repEmpty);
    assert(pickedEmpty == 1);
    assert(repEmpty.kind == AcquireKind::ReusedEmpty);

    // Commit slot 1 with seq 5 so all free slots (0, 1, 2) are now valid.
    ring.CommitRecord(1, 3840, 2160, false, 1050, w[1], r[1]);

    // 3. When multiple free slots match (slot 1 seq 5 and slot 2 seq 3), pick the oldest matching slot (slot 2).
    AcquireReport repOlderMatch {};
    size_t pickedOlderMatch = ring.AcquireForRecord(
        noReadInFlight, noWriteInFlight, [](size_t i) { return i == 1 || i == 2; }, &repOlderMatch);
    assert(pickedOlderMatch == 2);
    assert(repOlderMatch.kind == AcquireKind::ReusedMatching);

    // 4. When all free slots are populated and none match, evict the oldest mismatched slot (slot 0, seq 1).
    AcquireReport repEvict {};
    size_t pickedEvict =
        ring.AcquireForRecord(noReadInFlight, noWriteInFlight, [](size_t) { return false; }, &repEvict);
    assert(pickedEvict == 0);
    assert(repEvict.kind == AcquireKind::EvictedOldest);
}

// -------------------------------------------------------------------------------------------------
// Full Detail::Model simulation under DLSS-G 1x, 2x, 4x, same-queue and cross-queue (port of sim.cpp / drops.cpp)
// -------------------------------------------------------------------------------------------------
struct SimClone
{
    Usage writeUsage;
    Usage captureWriteUsage;
    Usage readUsage;
    Usage presentReadUsage;
    bool tracked = false;
    bool readTracked = false;
};

struct SimResult
{
    long capSkip = 0;
    long neutral = 0;
    long enhanced = 0;
    long dupSkip = 0;
    long drops = 0;
};

static SimResult RunSimulation(int frames, int B, size_t presentQ, unsigned lag, double dropP, unsigned seed, Model& m,
                               uint64_t* gpuDone, uint64_t* nextFence)
{
    m = Model {};
    for (size_t i = 0; i < MaxQueues; ++i)
    {
        gpuDone[i] = 0;
        nextFence[i] = 0;
    }

    auto completed = [gpuDone](size_t q) -> uint64_t { return gpuDone[q]; };
    auto isCompleted = [&](const Usage& u)
    {
        bool any = false;
        for (auto& e : u.epochs)
        {
            if (!e)
                continue;
            any = true;
            if (!IsComplete(*e, completed))
                return false;
        }
        return any;
    };
    auto isOnlyOn = [&](const Usage& u, size_t q)
    {
        bool any = false;
        for (auto& e : u.epochs)
        {
            if (!e)
                continue;
            any = true;
            if (!IsSubmittedOnlyOn(*e, q))
                return false;
        }
        return any;
    };
    auto submit = [&](uintptr_t list, size_t q)
    {
        auto e = m.BeforeExecute(list);
        Model::AfterExecute(e, q, ++nextFence[q], true);
    };
    auto reset = [&](uintptr_t list) { Model::AfterReset(m.Current(list), true); };

    SimClone clones[kRingSize];
    Ring ring;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<> U(0, 1);

    uint64_t lastEnhancedRecorded = 0;
    SimResult r;
    int64_t now = 1000;
    unsigned pslot = 0;
    uint64_t pfence[3] = {};

    for (int f = 0; f < frames; ++f)
    {
        // Game render: evaluate list reset, CaptureTemporal, submit on queue 0
        uintptr_t evalList = 100 + f % 3;
        reset(evalList);

        auto inFlight = [&](void* t)
        {
            auto* c = static_cast<SimClone*>(t);
            if (!t || !c->readTracked)
                return false;
            if (isCompleted(c->presentReadUsage))
            {
                c->readTracked = false;
                return false;
            }
            return true;
        };

        size_t s = ring.AcquireForRecord(
            inFlight, [&](void* token) { return CaptureBusy(token, gpuDone); }, [](size_t) { return false; });
        if (s >= kRingSize)
            ++r.capSkip;
        else
        {
            auto& c = clones[s];
            c.captureWriteUsage = {};
            c.tracked = (m.Track(evalList, c.writeUsage, completed) != nullptr) &&
                        (m.Track(evalList, c.captureWriteUsage, completed) != nullptr);
            assert(c.tracked && "write track must not refuse during active session");
            ring.CommitRecord(s, 1920, 1080, false, now, &c.captureWriteUsage, &c);
        }
        submit(evalList, 0);

        for (int p = 0; p < B; ++p)
        {
            uintptr_t pl = 200 + pslot;
            if (gpuDone[presentQ] < pfence[pslot])
                gpuDone[presentQ] = pfence[pslot];
            reset(pl);

            const auto newestRecorded = ring.NewestRecordedSequence();
            if (newestRecorded > 0 && newestRecorded == lastEnhancedRecorded)
            {
                // Sequence check BEFORE track: skip present without calling Track
                ++r.dupSkip;
                pslot = (pslot + 1) % 3;
                now += 16 / B;
                continue;
            }

            auto elig = [&](void* t)
            {
                auto& u = *static_cast<Usage*>(t);
                return isOnlyOn(u, presentQ) || isCompleted(u);
            };

            size_t pk = ring.PickForPresent(elig, now);
            bool tracked = false;
            Usage savedUsage;
            bool savedReadTracked = false;
            size_t ts = kRingSize;

            if (pk < kRingSize)
            {
                auto& c = clones[pk];
                savedUsage = c.readUsage;
                savedReadTracked = c.readTracked;
                c.presentReadUsage = {};
                tracked = (m.Track(pl, c.readUsage, completed) != nullptr) &&
                          (m.Track(pl, c.presentReadUsage, completed) != nullptr);
                if (tracked)
                {
                    c.readTracked = true;
                    ts = pk;
                    lastEnhancedRecorded = newestRecorded;
                    ++r.enhanced;
                }
                else
                {
                    c.readUsage = savedUsage;
                }
            }
            else
            {
                ++r.neutral;
            }

            if (ts < kRingSize && U(rng) < dropP)
            {
                // Unsubmitted present drop (e.g. g_nr.failed)
                ++r.drops;
                clones[ts].readUsage = savedUsage;
                clones[ts].readTracked = savedReadTracked;
                m.Abandon(pl); // Abandon cleans up epoch without leaking capacity
            }
            else
            {
                submit(pl, presentQ);
                pfence[pslot] = nextFence[presentQ];
            }

            pslot = (pslot + 1) % 3;
            for (size_t q : { (size_t) 0, presentQ })
                gpuDone[q] = nextFence[q] > lag ? std::max(gpuDone[q], nextFence[q] - lag) : gpuDone[q];
            now += 16 / B;
        }

        for (size_t q : { (size_t) 0, presentQ })
            gpuDone[q] = nextFence[q] > lag ? std::max(gpuDone[q], nextFence[q] - lag) : gpuDone[q];
    }

    return r;
}

void TestFullSubmissionModelSimulation()
{
    Model m;
    uint64_t gpuDone[MaxQueues] {};
    uint64_t nextFence[MaxQueues] {};

    // 1. Same-queue 1x, lag 2: 20000 / 20000 frames enhanced, zero skips, zero neutral
    auto r1 = RunSimulation(20000, 1, 0, 2, 0.0, 7, m, gpuDone, nextFence);
    assert(r1.enhanced == 20000 && r1.neutral == 0 && r1.capSkip == 0 && r1.drops == 0);

    // 2. Same-queue DLSS-G 2x, lag 2: 20000 enhanced, 20000 duplicate skips
    auto r2 = RunSimulation(20000, 2, 0, 2, 0.0, 7, m, gpuDone, nextFence);
    assert(r2.enhanced == 20000 && r2.dupSkip == 20000 && r2.neutral == 0 && r2.capSkip == 0);

    // 3. Same-queue DLSS-G 4x, lag 3: 20000 enhanced, 60000 duplicate skips
    auto r3 = RunSimulation(20000, 4, 0, 3, 0.0, 7, m, gpuDone, nextFence);
    assert(r3.enhanced == 20000 && r3.dupSkip == 60000 && r3.neutral == 0 && r3.capSkip == 0);

    // 4. Same-queue DLSS-G 3x with 5% unsubmitted list drops:
    // With m.Abandon(pl), all dropped epochs are immediately recycled; exactly 20000 frames enhanced!
    auto r4 = RunSimulation(20000, 3, 0, 2, 0.05, 7, m, gpuDone, nextFence);
    assert(r4.enhanced == 20000 && r4.dupSkip == 40000 && r4.neutral == 0 && r4.drops > 900);

    // Verify model capacity after 20,000 frames with drops: fresh epochs are allocatable
    auto completed = [&gpuDone](size_t q) -> uint64_t { return gpuDone[q]; };
    int allocatable = 0;
    for (int i = 0; i < 300; ++i)
    {
        Usage one;
        if (m.Track(5000 + i, one, completed))
            ++allocatable;
    }
    assert(allocatable >= 250 && "Model epochs must not leak after dropped present lists");

    // 5. Cross-queue DLSS-G 2x, lag 2: 19998 enhanced
    auto r5 = RunSimulation(20000, 2, 1, 2, 0.0, 7, m, gpuDone, nextFence);
    assert(r5.enhanced >= 19990 && r5.capSkip == 0);
}

int main()
{
    TestCaptureReuseAndCancelledRing();
    TestRingBasicAcquireCommitPick();
    TestRingInFlightReadProtectionNotLastRead();
    TestRingAllSlotsInFlightSkipsCapture();
    TestRingMaxAgeStaleRejection();
    TestProductionSignature();
    TestRingInFlightWriteProtectionAndBusyReport();
    TestRingShapePreferenceOrder();
    TestFullSubmissionModelSimulation();
    std::printf(
        "PASS: nr-present-guides ring acquisition, in-flight read protection, skip fallback, max-age and 20k-frame "
        "DLSS-G Model simulation\n");
    return 0;
}
