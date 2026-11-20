#include "pch.h"

// Verbatim production admission/lifetime controller; no D3D12 or NGX execution.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <string>
#include <vector>

// Every allocation in the process is counted, so the default one-pass path can be shown to make none.
static size_t g_allocations = 0;
void* operator new(std::size_t size)
{
    ++g_allocations;
    if (void* memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

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
// Every call into the tracker: Track installs the queue and Reset hooks, and all three take its state lock.
int tracks = 0, chainTracks = 0, locks = 0;
bool Strict()
{
    ++locks;
    return false;
}
Detail::Admission Admit(const Usage&, ID3D12GraphicsCommandList*)
{
    ++locks;
    return Detail::Admission::Admit;
}
bool Track(ID3D12GraphicsCommandList*, Usage&, bool chain)
{
    ++tracks;
    ++locks;
    chainTracks += chain;
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
uint64_t g_nrTicks = 1;
bool adaptive = false, retirementComplete = false, proxy = false, fits = true;
int parks = 0, ticks = 0, retains = 0, budgetAsks = 0, proxyAsks = 0, statuses = 0;
std::string status;
bool AdaptiveCadenceRequested() { return adaptive; }
bool DriverProxyRequested()
{
    ++proxyAsks;
    return proxy;
}
void ChainStatus(const char* text)
{
    ++statuses;
    status = text;
}
// The adapter query (DXGI) and the detach are production code outside this slice; the detach's collector and
// inventory are checked by cases.cpp and run.py. Here they are counted, and the detach must find tracking off.
bool SecondGenerationFits(ID3D12GraphicsCommandList*)
{
    ++budgetAsks;
    return fits;
}
void RetainUntrackedGeneration(ID3D12GraphicsCommandList*)
{
    assert(!g_tracked);
    ++retains;
}
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
    g_tracked = g_legacyRecorded = g_haveTrackedUsage = adaptive = retirementComplete = proxy = false;
    fits = true;
    s_trackingTransition = s_generationParked = s_legacyExternalRecording = false;
    s_legacyAfterUpscale = s_legacyPrivatePresent = s_legacyOtherRoute = false;
    s_generationRetained = false;
    s_retainRefusal = nullptr;
    s_retainRefusedAt = 0;
    g_nrTicks = 1;
    retains = budgetAsks = proxyAsks = statuses = 0;
    DlssNr::Submission::chainTracks = DlssNr::Submission::locks = 0;
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
    constexpr auto Private = NrRecordingRoute::PrivatePresent;
    constexpr auto Upscaled = NrRecordingRoute::AfterUpscale;
    constexpr auto Other = NrRecordingRoute::Other;
    auto record = [&](DlssNrPassSnapshot request, bool own = true, bool mandatory = false)
    {
        lease.tracked = false;
        return TrackNrRecording(&second, request, &lease, mandatory, own ? Private : Other);
    };
    auto upscaled =
        [&](DlssNrPassSnapshot request, NrRecordingRoute route = NrRecordingRoute::AfterUpscale, bool mandatory = false)
    {
        lease.tracked = false;
        ++g_nrTicks; // every NR entry advances the clock before admission
        return TrackNrRecording(&second, request, &lease, mandatory, route);
    };
    const auto says = [&](const char* text) { return status.find(text) != std::string::npos; };

    // Default one/master: no tracker, retirement, or migration. Disabled never calls admission.
    Reset();
    assert(!WantsTrackedRecording(one, false));
    assert(TrackNrRecording(&first, one, nullptr, false, Private));
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
    assert(TrackNrRecording(&first, one, nullptr, false, Other));
    assert(record(many) && !g_tracked && parks == 0);
    assert(status.find("Restart required") != std::string::npos);
    assert(!record(many, false, true) && parks == 0);

    // An invalid lease cannot park or arm tracking after legacy work.
    Reset();
    assert(TrackNrRecording(&first, one, nullptr, false, Private));
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
        assert(TrackNrRecording(&first, one, nullptr, false, Private));
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
    assert(TrackNrRecording(&first, one, nullptr, false, Private));
    EndUntrackedPresent(&first, nullptr, nullptr, 0);
    assert(!record(one, false, true) && parks == 1);
    retirementComplete = true;
    assert(record(one, false, true) && g_tracked);

    // A late submission on another queue invalidates the pending transition before it parks anything.
    Reset();
    assert(TrackNrRecording(&first, one, nullptr, false, Private));
    EndUntrackedPresent(&first, &queue, &fence, 3);
    assert(TrackNrRecording(&first, one, nullptr, false, Private));
    assert(!record(many) && parks == 0);
    EndUntrackedPresent(&first, &otherQueue, &fence, 4);
    assert(!record(many) && parks == 0 && !g_tracked);
    assert(status.find("Restart required") != std::string::npos);
    assert(!UntrackedPresentCompleted());

    // ---- Live opt-in from the upscaler's output (chain-in-flight.md) ----

    // The default one-pass path on that route gains nothing: a thousand recordings make no tracker call (so no
    // hook is installed and no tracker lock is taken), ask the adapter nothing, park and collect nothing, write no
    // status, and allocate nothing. Only the provenance flags move.
    Reset();
    assert(!WantsTrackedRecording(one, false));
    const size_t allocationsBefore = g_allocations;
    for (int frame = 0; frame != 1000; ++frame)
        assert(TrackNrRecording(frame % 2 ? &first : &second, one, nullptr, false, Upscaled));
    assert(g_allocations == allocationsBefore);
    assert(g_legacyRecorded && !g_tracked && !GenerationRetained() && !TrackingTransitionPending());
    assert(DlssNr::Submission::tracks == 0 && DlssNr::Submission::locks == 0);
    assert(budgetAsks == 0 && proxyAsks == 0 && retains == 0 && parks == 0 && ticks == 0 && statuses == 0);
    assert(g_nrRetired.empty() && !lease.tracked);
    assert(s_legacyAfterUpscale && !s_legacyPrivatePresent && !s_legacyOtherRoute && s_legacyExternalRecording);

    // Raising the count: the adapter and the tracker are asked once, the generation is detached while tracking is
    // still off, tracking is armed in the same call, and that call records nothing.
    assert(!upscaled(many));
    assert(retains == 1 && budgetAsks == 1 && DlssNr::Submission::tracks == 1 && DlssNr::Submission::chainTracks == 0);
    assert(g_tracked && !g_legacyRecorded && GenerationRetained() && !TrackingTransitionPending());
    assert(parks == 0 && ticks == 0 && !lease.tracked && says("previous model stays in video memory"));
    // The next call is an ordinary tracked recording on fresh objects.
    assert(upscaled(many) && lease.tracked && DlssNr::Submission::chainTracks == 1 && retains == 1);
    // Back to one pass and up again: tracking is sticky, so nothing is left behind a second time.
    assert(upscaled(one) && g_tracked && lease.tracked);
    assert(upscaled(many) && retains == 1 && budgetAsks == 1 && GenerationRetained());
    // Defence in depth. No production path turns tracking off again; were one to, a second untracked generation
    // would still not be left behind: the old restart status, and one pass.
    g_tracked = false;
    assert(upscaled(one) && g_legacyRecorded);
    assert(upscaled(many) && retains == 1 && !g_tracked && says("Restart required"));

    // A second generation that does not fit beside the first: one pass keeps running, nothing is detached, the
    // tracker is not touched, and the status is not the restart one. The adapter is asked again only after the
    // retry interval, not on every frame the request stands.
    Reset();
    assert(upscaled(one));
    fits = false;
    assert(upscaled(many) && !g_tracked && g_legacyRecorded && retains == 0 && DlssNr::Submission::tracks == 0);
    assert(says("Not enough video memory") && says("One pass keeps running") && !says("Restart required"));
    assert(budgetAsks == 1 && !GenerationRetained());
    for (int frame = 0; frame != 100; ++frame)
        assert(upscaled(many) && retains == 0);
    assert(budgetAsks == 1);
    fits = true; // memory came back, but the answer stands until the interval has passed
    assert(upscaled(many) && retains == 0 && budgetAsks == 1);
    g_nrTicks += 128;
    assert(!upscaled(many) && retains == 1 && budgetAsks == 2 && g_tracked);

    // A runtime whose lists the tracker cannot follow: the same, with its own words, before anything is detached.
    Reset();
    assert(upscaled(one));
    DlssNr::Submission::trackOk = false;
    assert(upscaled(many) && !g_tracked && retains == 0 && DlssNr::Submission::tracks == 1);
    assert(says("cannot be followed") && says("One pass keeps running"));
    DlssNr::Submission::trackOk = true;
    g_nrTicks += 128;
    assert(!upscaled(many) && retains == 1 && g_tracked);

    // The driver proxy runs one pass whatever is asked: nothing is left behind for it, and nothing is asked.
    Reset();
    assert(upscaled(one));
    proxy = true;
    assert(upscaled(many) && !g_tracked && retains == 0 && budgetAsks == 0 && DlssNr::Submission::tracks == 0);
    assert(says("Driver proxy"));
    proxy = false;
    assert(!upscaled(many) && retains == 1);

    // Another recording scope owns the resources: nothing is detached under it; one pass this frame.
    Reset();
    assert(upscaled(one));
    lease.valid = false;
    assert(upscaled(many) && !g_tracked && retains == 0 && budgetAsks == 0 && says("transition deferred"));
    lease.valid = true;
    assert(!upscaled(many) && retains == 1);

    // Only a history made on the upscaler's output alone. A bridge or before-upscale recording in it (Other), or a
    // private present one, keeps the restart status; so does an opt-in that arrives on another route.
    for (const auto extra : { Other, Private })
    {
        Reset();
        assert(upscaled(one));
        assert(TrackNrRecording(&first, one, nullptr, false, extra));
        assert(upscaled(many) && !g_tracked && retains == 0 && budgetAsks == 0 && says("Restart required"));
    }
    for (const auto arrival : { Other, Private })
    {
        Reset();
        assert(upscaled(one));
        assert(upscaled(many, arrival) && !g_tracked && retains == 0 && budgetAsks == 0 && says("Restart required"));
    }
    Reset();
    assert(TrackNrRecording(&first, one, nullptr, false, Other)); // the bridge's present host alone
    assert(upscaled(many, Other) && !g_tracked && retains == 0 && says("Restart required"));

    // Mandatory Dx11SceneStage still refuses that history outright.
    Reset();
    assert(upscaled(one));
    assert(!upscaled(one, Other, true) && !g_tracked && retains == 0 && says("Dx11SceneStage"));
    assert(!upscaled(one, Upscaled, true) && !g_tracked && retains == 0 && says("Dx11SceneStage"));

    // Individual settings and adaptive cadence opt in the same way.
    for (int kind = 0; kind != 2; ++kind)
    {
        Reset();
        assert(upscaled(one));
        DlssNrPassSnapshot request = one;
        request.Individual = kind == 0;
        adaptive = kind == 1;
        assert(!upscaled(request) && retains == 1 && g_tracked);
        assert(upscaled(request) && lease.tracked);
    }

    // A session that starts above one pass on this route is tracked from its first recording, as before.
    Reset();
    assert(upscaled(many) && g_tracked && lease.tracked && retains == 0 && budgetAsks == 0 && !GenerationRetained());
}
