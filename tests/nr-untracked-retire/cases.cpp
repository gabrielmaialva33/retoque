#include <dlssnr/DlssNr_UntrackedRetire.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace
{
using DlssNr::UntrackedRetire::CanFree;
using DlssNr::UntrackedRetire::DescribeUnavailable;
using DlssNr::UntrackedRetire::NextSignalValue;
using DlssNr::UntrackedRetire::Query;
using DlssNr::UntrackedRetire::StepCountdown;
using DlssNr::UntrackedRetire::UnavailableReason;

void TestPureDecisionRule()
{
    // StepCountdown walks down to 0 and never underflows across long retentions.
    assert(StepCountdown(32) == 31);
    assert(StepCountdown(1) == 0);
    assert(StepCountdown(0) == 0);
    assert(StepCountdown(-5) == 0);

    // NextSignalValue advances monotonically and reserves UINT64_MAX for device removal.
    assert(NextSignalValue(0) == 1);
    assert(NextSignalValue(41) == 42);
    assert(NextSignalValue(UINT64_MAX - 3) == UINT64_MAX - 2);
    assert(NextSignalValue(UINT64_MAX - 2) == UINT64_MAX - 1);
    assert(NextSignalValue(UINT64_MAX - 1) == UINT64_MAX - 1);
    assert(NextSignalValue(UINT64_MAX) == UINT64_MAX - 1);

    // Every UnavailableReason has a non-empty description.
    for (UnavailableReason r :
         { UnavailableReason::None, UnavailableReason::NoSwapchainPresent, UnavailableReason::MultiplePresentQueues,
           UnavailableReason::SlotLimitReached, UnavailableReason::CreateFenceFailed, UnavailableReason::SignalFailed })
    {
        const char* text = DescribeUnavailable(r);
        assert(text != nullptr && std::strlen(text) > 0);
    }

    // 1. Unexpired count gate never frees, even when fence completed or no fence exists.
    {
        Query done {
            .hasFence = true, .deviceRemoved = false, .unavailable = UnavailableReason::None, .completedValue = 99
        };
        Query noFence { .hasFence = false,
                        .deviceRemoved = false,
                        .unavailable = UnavailableReason::NoSwapchainPresent };
        assert(!CanFree(31, 10, done));
        assert(!CanFree(1, 10, done));
        assert(!CanFree(1, 10, noFence));
    }

    // 2. Active fence: requires completedValue >= parkedSerial + 1 (one full Present after park).
    {
        Query q {
            .hasFence = true, .deviceRemoved = false, .unavailable = UnavailableReason::None, .completedValue = 0
        };
        assert(!CanFree(0, 10, q));
        q.completedValue = 9;
        assert(!CanFree(0, 10, q));
        q.completedValue = 10; // only the Present that was next at park, not a full cycle after park
        assert(!CanFree(0, 10, q));
        q.completedValue = 11; // parkedSerial + 1
        assert(CanFree(0, 10, q));
        q.completedValue = 500;
        assert(CanFree(0, 10, q));
    }

    // 3. UINT64_MAX (DXGI_ERROR_DEVICE_REMOVED) and deviceRemoved never count as completion or fallback.
    {
        Query sentinel { .hasFence = true,
                         .deviceRemoved = false,
                         .unavailable = UnavailableReason::None,
                         .completedValue = UINT64_MAX };
        assert(!CanFree(0, 0, sentinel));
        assert(!CanFree(0, 1, sentinel));
        assert(!CanFree(0, 100, sentinel));

        Query removedNoFence { .hasFence = false,
                               .deviceRemoved = true,
                               .unavailable = UnavailableReason::CreateFenceFailed,
                               .completedValue = 0 };
        assert(!CanFree(0, 1, removedNoFence));

        Query removedWithFence {
            .hasFence = true, .deviceRemoved = true, .unavailable = UnavailableReason::None, .completedValue = 999
        };
        assert(!CanFree(0, 1, removedWithFence));
    }

    // 4. Saturation near UINT64_MAX never wraps parkedSerial + 1 to 0 or confuses UINT64_MAX with completion.
    {
        Query maxValid { .hasFence = true,
                         .deviceRemoved = false,
                         .unavailable = UnavailableReason::None,
                         .completedValue = UINT64_MAX - 1 };
        assert(CanFree(0, UINT64_MAX - 2, maxValid));
        assert(!CanFree(0, UINT64_MAX - 1, maxValid));
        assert(!CanFree(0, UINT64_MAX, maxValid));

        Query sentinel { .hasFence = true,
                         .deviceRemoved = false,
                         .unavailable = UnavailableReason::None,
                         .completedValue = UINT64_MAX };
        assert(!CanFree(0, UINT64_MAX - 2, sentinel));
        assert(!CanFree(0, UINT64_MAX - 1, sentinel));
        assert(!CanFree(0, UINT64_MAX, sentinel));
    }

    // 5. Live device with no present-queue fence available falls back to count-only once framesLeft reaches 0.
    for (UnavailableReason reason :
         { UnavailableReason::NoSwapchainPresent, UnavailableReason::MultiplePresentQueues,
           UnavailableReason::SlotLimitReached, UnavailableReason::CreateFenceFailed, UnavailableReason::SignalFailed })
    {
        Query fallback { .hasFence = false, .deviceRemoved = false, .unavailable = reason, .completedValue = 0 };
        assert(!CanFree(1, 7, fallback));
        assert(CanFree(0, 7, fallback));
    }
}
} // namespace

// --- Production lifetime slice harness -----------------------------------------------------------------------

namespace
{
std::vector<std::string> g_logs;
#define LOG_INFO(...) g_logs.push_back(std::format(__VA_ARGS__))
#define LOG_WARN(...) g_logs.push_back("WARN " + std::format(__VA_ARGS__))

struct ID3D12Resource
{
    unsigned releases = 0;
    void Release() { ++releases; }
};
struct OS_Dx12
{
};
struct DlssNr_Stabilizer_Dx12
{
};
struct DlssNr_UiMask_Dx12
{
};
struct DlssNr_Cadence_Dx12
{
};
struct NrEnlarger
{
};
struct DlssNr_DetailStats_Dx12
{
};
struct DlssNr_Pulse_Dx12
{
};
struct DlssNr_MotionProbe_Dx12
{
};
} // namespace

namespace DlssNr::Submission
{
struct Usage
{
};
uint64_t Clock() { return 0; }
bool Ready(const Usage&) { return false; }
bool Settled(const Usage&) { return false; }
bool OnlyUnanswerableLeft(const Usage&) { return false; }
namespace Detail
{
constexpr uint64_t AgeOutTicks = 128;
}
} // namespace DlssNr::Submission

namespace
{
struct FakePresentFence
{
    uint64_t nextSerial = 1;
    bool hasFence = true;
    bool deviceRemoved = false;
    UnavailableReason unavailable = UnavailableReason::None;
    uint64_t completedValue = 0;
    unsigned queries = 0;
    bool fallbackLogged = false;
} g_fakeFence;

uint64_t SnapshotUntrackedPresentSerial() { return g_fakeFence.nextSerial; }

void QueryUntrackedPresentFence(bool* hasFence, bool* deviceRemoved, uint8_t* unavailableReason,
                                uint64_t* completedValue)
{
    ++g_fakeFence.queries;
    *hasFence = g_fakeFence.hasFence;
    *deviceRemoved = g_fakeFence.deviceRemoved;
    *unavailableReason = static_cast<uint8_t>(g_fakeFence.unavailable);
    *completedValue = g_fakeFence.completedValue;
}

void LogUntrackedPresentFenceUnavailableOnce(uint8_t unavailableReason)
{
    if (!g_fakeFence.fallbackLogged)
    {
        g_fakeFence.fallbackLogged = true;
        const auto reason = static_cast<UnavailableReason>(unavailableReason);
        LOG_INFO("DLSS-NR retirement: untracked D3D12 GPU-completion fence unavailable ({}); using evaluation-count "
                 "retirement only",
                 DescribeUnavailable(reason));
    }
}

bool g_tracked = false;
DlssNr::Submission::Usage g_usage;
unsigned g_releasedFeatures = 0;
void FakeReleaseFeature(void*) { ++g_releasedFeatures; }

struct
{
    void (*release)(void*) = &FakeReleaseFeature;
} g_nr;

struct PresentGuideCloneStub
{
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    uint64_t depthBytes = 0;
    uint64_t motionBytes = 0;
};
PresentGuideCloneStub g_presentGuideClones[1] {};

#include "artifacts/lifetime_slice.inc"

void ResetHarness()
{
    g_nrRetired.clear();
    g_logs.clear();
    g_tracked = false;
    g_releasedFeatures = 0;
    g_fakeFence = FakePresentFence {};
}

void TestProductionLifetimeSlice()
{
    // 1. Fence completes early: still retained until the 32nd tick, never freed early, queried only on tick 32.
    ResetHarness();
    g_fakeFence.nextSerial = 20;
    g_fakeFence.completedValue = 25; // already >= 21
    ID3D12Resource resA {};
    ID3D12Resource* ptrA = &resA;
    ParkNrResource(ptrA);
    assert(ptrA == nullptr && g_nrRetired.size() == 1 && g_nrRetired[0].presentSerial == 20);
    for (int tick = 1; tick <= 31; ++tick)
    {
        TickNrRetired(false);
        assert(resA.releases == 0 && g_fakeFence.queries == 0);
    }
    TickNrRetired(false);
    assert(resA.releases == 1 && g_nrRetired.empty() && g_fakeFence.queries == 1 && g_logs.empty());

    // 2. Count expires while GPU is behind: retained past tick 32 until completedValue reaches parkedSerial + 1;
    //    multiple expired entries share one fence query per tick.
    ResetHarness();
    g_fakeFence.nextSerial = 50;
    g_fakeFence.completedValue = 49;
    ID3D12Resource resB {}, resC {};
    ID3D12Resource* ptrB = &resB;
    ID3D12Resource* ptrC = &resC;
    void* feature = reinterpret_cast<void*>(0x1234);
    ParkNrResource(ptrB);
    ParkNrResource(ptrC);
    ParkNrFeature(feature);
    assert(g_nrRetired.size() == 3);
    for (int tick = 1; tick <= 32; ++tick)
        TickNrRetired(false);
    assert(resB.releases == 0 && resC.releases == 0 && g_releasedFeatures == 0);
    assert(g_fakeFence.queries == 1); // queried once for all three entries on tick 32

    g_fakeFence.completedValue = 50; // still == parkedSerial, one short of parkedSerial + 1
    for (int extra = 0; extra < 10; ++extra)
        TickNrRetired(false);
    assert(resB.releases == 0 && resC.releases == 0 && g_releasedFeatures == 0);
    assert(g_nrRetired[0].framesLeft == 0); // clamped at 0, never negative

    g_fakeFence.completedValue = 51; // parkedSerial + 1 reached
    const unsigned queriesBefore = g_fakeFence.queries;
    TickNrRetired(false);
    assert(g_fakeFence.queries == queriesBefore + 1);
    assert(resB.releases == 1 && resC.releases == 1 && g_releasedFeatures == 1 && g_nrRetired.empty());

    // 3. GetCompletedValue() == UINT64_MAX (device removed): retained indefinitely, no fallback log.
    ResetHarness();
    g_fakeFence.nextSerial = 5;
    g_fakeFence.completedValue = UINT64_MAX;
    ID3D12Resource lostRes {};
    ID3D12Resource* lostPtr = &lostRes;
    ParkNrResource(lostPtr);
    for (int tick = 0; tick < 100; ++tick)
        TickNrRetired(false);
    assert(lostRes.releases == 0 && g_nrRetired.size() == 1 && g_logs.empty());

    // 4. Device removed without a usable fence (e.g. CreateFence failed on lost device): retained, no fallback log.
    ResetHarness();
    g_fakeFence.hasFence = false;
    g_fakeFence.deviceRemoved = true;
    g_fakeFence.unavailable = UnavailableReason::CreateFenceFailed;
    ID3D12Resource lostCreateRes {};
    ID3D12Resource* lostCreatePtr = &lostCreateRes;
    ParkNrResource(lostCreatePtr);
    for (int tick = 0; tick < 64; ++tick)
        TickNrRetired(false);
    assert(lostCreateRes.releases == 0 && g_nrRetired.size() == 1 && g_logs.empty());

    // 5. No fence available on a live device: exact 32-tick count-only release, logged once at INFO.
    ResetHarness();
    g_fakeFence.hasFence = false;
    g_fakeFence.deviceRemoved = false;
    g_fakeFence.unavailable = UnavailableReason::MultiplePresentQueues;
    ID3D12Resource fb1 {}, fb2 {};
    ID3D12Resource* fbPtr1 = &fb1;
    ID3D12Resource* fbPtr2 = &fb2;
    ParkNrResource(fbPtr1);
    for (int tick = 1; tick <= 31; ++tick)
        TickNrRetired(false);
    assert(fb1.releases == 0 && g_logs.empty());
    TickNrRetired(false);
    assert(fb1.releases == 1 && g_nrRetired.empty() && g_logs.size() == 1);
    assert(g_logs[0].find("more than one present queue") != std::string::npos);

    ParkNrClone(fbPtr2, DlssNr::Submission::Usage {}, false);
    for (int tick = 1; tick <= 32; ++tick)
        TickNrRetired(false);
    assert(fb2.releases == 1 && g_logs.size() == 1); // still logged once

    // 6. Shutdown path (shutdown=true): preserves original countdown behavior without querying the present fence.
    ResetHarness();
    g_fakeFence.completedValue = 0; // would block normal untracked retirement
    ID3D12Resource sdRes {};
    ID3D12Resource* sdPtr = &sdRes;
    ParkNrResource(sdPtr);
    for (int tick = 1; tick <= 32; ++tick)
        TickNrRetired(true);
    assert(sdRes.releases == 1 && g_fakeFence.queries == 0);
}
} // namespace

int main()
{
    TestPureDecisionRule();
    TestProductionLifetimeSlice();
    return 0;
}
