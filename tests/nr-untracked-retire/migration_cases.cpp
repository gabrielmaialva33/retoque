#include "pch.h"

// Verbatim production admission/lifetime controller; no D3D12 or NGX execution.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#define LOG_WARN(...) ((void) 0)
struct PresentList
{
    static constexpr unsigned kSlots = 4;
};
struct ID3D12GraphicsCommandList
{
};
struct ID3D12CommandQueue
{
    int refs = 0;
    void AddRef() { ++refs; }
};
struct ID3D12Fence
{
    int refs = 0;
    uint64_t completed = 0;
    void AddRef() { ++refs; }
    uint64_t GetCompletedValue() { return completed; }
};
struct DlssNrPassSnapshot
{
    unsigned Count = 1;
    bool Individual = false;
};
namespace DlssNr::Submission
{
struct Usage
{
};
namespace Detail
{
enum class Admission
{
    Admit,
    Pending
};
}
bool trackOk = true;
int tracks = 0;
bool Strict() { return false; }
Detail::Admission Admit(const Usage&, ID3D12GraphicsCommandList*) { return Detail::Admission::Admit; }
bool Track(ID3D12GraphicsCommandList*, Usage&, bool)
{
    ++tracks;
    return trackOk;
}
const char* LastRefusal() { return "unsupported runtime"; }
} // namespace DlssNr::Submission
namespace DlssNr::Chain
{
struct RecordingLease
{
    bool valid = true, tracked = false;
    bool Valid(int, uintptr_t) const { return valid; }
    bool Tracked() const { return tracked; }
    bool MayTrack(bool previous, bool admitted) const { return !previous || admitted; }
    void MarkTracked() { tracked = true; }
};
} // namespace DlssNr::Chain
struct NrRetired
{
    bool requiresPresentFence = true;
};
std::vector<NrRetired> g_nrRetired;
bool g_tracked = false, g_legacyRecorded = false, g_haveTrackedUsage = false;
int g_recordings = 0;
std::mutex g_nrMutex;
DlssNr::Submission::Usage g_usage;
bool adaptive = false, retirementComplete = false;
int parks = 0, ticks = 0;
std::string status;
bool AdaptiveCadenceRequested() { return adaptive; }
void ChainStatus(const char* text) { status = text; }
const char* AdmissionRefusal(DlssNr::Submission::Detail::Admission) { return "admission refused"; }
bool RetiredCapacity() { return true; }
// Resource detach/collection is tested separately by real Park*/Tick slices and a full inventory guard.
void ParkUntrackedGeneration()
{
    assert(!g_tracked);
    ++parks;
    g_nrRetired.push_back({ true });
}
void TickNrRetired()
{
    assert(!g_tracked);
    ++ticks;
    if (retirementComplete)
        g_nrRetired.clear();
}
#include "migration.inc"

void Reset()
{
    g_tracked = g_legacyRecorded = g_haveTrackedUsage = adaptive = retirementComplete = false;
    s_trackingTransition = s_generationParked = s_legacyExternalRecording = false;
    s_legacyPending = {};
    s_legacyQueue = nullptr;
    s_legacyFence = nullptr;
    s_legacyFenceValue = 0;
    g_nrRetired.clear();
    DlssNr::Submission::trackOk = true;
    DlssNr::Submission::tracks = parks = ticks = 0;
    status.clear();
}

int main()
{
    ID3D12GraphicsCommandList first, second;
    ID3D12CommandQueue queue, otherQueue;
    ID3D12Fence fence;
    DlssNrPassSnapshot one, many { 10, false };
    DlssNr::Chain::RecordingLease lease;
    auto record = [&](DlssNrPassSnapshot request, bool own = true, bool mandatory = false)
    {
        lease.tracked = false;
        return TrackNrRecording(&second, request, &lease, mandatory, own);
    };

    // Default one/master: no tracker, retirement, or migration. Disabled never calls admission.
    Reset();
    assert(!WantsTrackedRecording(one, false));
    assert(TrackNrRecording(&first, one, nullptr, false, true));
    assert(g_legacyRecorded && !g_tracked && parks == 0 && ticks == 0 && DlssNr::Submission::tracks == 0);

    // An old private recording is still on the CPU. A request stops recording, but cannot park yet.
    assert(WantsTrackedRecording(many, false));
    assert(!record(many));
    assert(TrackingTransitionPending() && parks == 0 && DlssNr::Submission::tracks == 0);
    assert(!record(one)); // reverting the slider during migration cannot resume untracked writes
    assert(parks == 0);
    EndUntrackedPresent(&first, &queue, &fence, 7);
    assert(!record(many));
    assert(parks == 1 && ticks == 0 && !g_tracked);
    retirementComplete = true;
    fence.completed = 6;
    assert(!record(many) && ticks == 0 && !g_tracked);
    fence.completed = UINT64_MAX;
    assert(!record(many) && ticks == 0 && !g_tracked);
    fence.completed = 7;
    retirementComplete = false;
    assert(!record(many) && parks == 1 && ticks == 1 && !g_tracked);
    retirementComplete = true;
    assert(record(many) && g_tracked && !g_legacyRecorded && !TrackingTransitionPending());
    assert(parks == 1 && DlssNr::Submission::tracks == 1 && lease.tracked);
    assert(record(one) && g_tracked); // sticky after success

    // Arbitrary game/bridge history remains single/master; mandatory scene-stage refuses.
    Reset();
    assert(TrackNrRecording(&first, one, nullptr, false, false));
    assert(record(many) && !g_tracked && parks == 0);
    assert(status.find("Restart required") != std::string::npos);
    assert(!record(many, false, true) && parks == 0);

    // An invalid lease cannot park or arm tracking after legacy work.
    Reset();
    assert(TrackNrRecording(&first, one, nullptr, false, true));
    EndUntrackedPresent(&first, nullptr, nullptr, 0);
    lease.valid = false;
    assert(!record(many) && parks == 0 && !TrackingTransitionPending());
    lease.valid = true;

    // Cancelled lists are not awaited on the GPU. Still wait on strict retirement, and fail closed if Track fails.
    assert(!record(many) && parks == 1);
    retirementComplete = true;
    DlssNr::Submission::trackOk = false;
    assert(!record(many) && g_tracked && !lease.tracked);
    DlssNr::Submission::trackOk = true;
    assert(record(many));

    // All opt-ins use the migration: individual settings, adaptive cadence; mandatory without legacy is unchanged.
    for (int kind = 0; kind != 2; ++kind)
    {
        Reset();
        assert(TrackNrRecording(&first, one, nullptr, false, true));
        EndUntrackedPresent(&first, nullptr, nullptr, 0);
        DlssNrPassSnapshot request = one;
        request.Individual = kind == 0;
        adaptive = kind == 1;
        assert(WantsTrackedRecording(request, false));
        assert(!record(request) && parks == 1);
        retirementComplete = true;
        assert(record(request) && g_tracked);
    }
    Reset();
    assert(record(one, false, true) && g_tracked && parks == 0);

    // The destination may be scene-stage/game-owned: only the OLD history needs private-host evidence.
    Reset();
    assert(TrackNrRecording(&first, one, nullptr, false, true));
    EndUntrackedPresent(&first, nullptr, nullptr, 0);
    assert(!record(one, false, true) && parks == 1);
    retirementComplete = true;
    assert(record(one, false, true) && g_tracked);

    // A late submission on another queue invalidates the pending transition before it parks anything.
    Reset();
    assert(TrackNrRecording(&first, one, nullptr, false, true));
    EndUntrackedPresent(&first, &queue, &fence, 3);
    assert(TrackNrRecording(&first, one, nullptr, false, true));
    assert(!record(many) && parks == 0);
    EndUntrackedPresent(&first, &otherQueue, &fence, 4);
    assert(!record(many) && parks == 0 && !g_tracked);
    assert(status.find("Restart required") != std::string::npos);
    assert(!UntrackedPresentCompleted());
}
