#include <mutex>
// The D3D12 present lists against a fence that cannot prove completion: the pure rule, then the production helpers
// (sliced verbatim out of DlssNr_Dx12.cpp by run.py) over fake D3D12 objects. One scenario per process, because the
// helpers latch a removal and their "said once" flags for the session.

#include <dlssnr/DlssNr_FenceProgress.h>
#include <dlssnr/DlssNr_PresentCpuTiming.h>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// --- the pure rule ------------------------------------------------------------------------------------------------

namespace
{
using DlssNr::FenceProgress::Classify;
using DlssNr::FenceProgress::Completed;
using DlssNr::FenceProgress::State;

// Compile-time for the header as it is; run.py's mutants of it skip these and have to be caught by a scenario.
#ifndef NR_PRESENT_FENCE_MUTANT
static_assert(Classify(0, 0) == State::Completed);
static_assert(Classify(4, 5) == State::Pending);
static_assert(Classify(5, 5) == State::Completed);
static_assert(Classify(9, 5) == State::Completed);
// UINT64_MAX is the removed device's answer, never a value a queue reached: whatever was asked for, zero included.
static_assert(Classify(UINT64_MAX, 0) == State::Removed);
static_assert(Classify(UINT64_MAX, 5) == State::Removed);
static_assert(Classify(UINT64_MAX, UINT64_MAX) == State::Removed);
// The largest value a queue can be asked to signal is still an ordinary one.
static_assert(Classify(UINT64_MAX - 1, UINT64_MAX - 1) == State::Completed);
static_assert(Classify(UINT64_MAX - 2, UINT64_MAX - 1) == State::Pending);
static_assert(Completed(5, 5) && Completed(9, 5) && !Completed(4, 5) && !Completed(UINT64_MAX, 5) &&
              !Completed(UINT64_MAX, 0));
#endif
} // namespace

// --- the platform the helpers see ---------------------------------------------------------------------------------

namespace
{
using HRESULT = int32_t;
using UINT64 = uint64_t;
using DWORD = uint32_t;
using UINT = unsigned int;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005u);
constexpr HRESULT DXGI_ERROR_DEVICE_REMOVED = static_cast<HRESULT>(0x887A0005u);
constexpr DWORD WAIT_OBJECT_0 = 0;
constexpr DWORD WAIT_TIMEOUT = 258;
constexpr DWORD WAIT_FAILED = 0xFFFFFFFFu;
constexpr int FALSE = 0;
constexpr int D3D12_COMMAND_LIST_TYPE_DIRECT = 0;
constexpr int D3D12_FENCE_FLAG_NONE = 0;
#define FAILED(hr) (static_cast<HRESULT>(hr) < 0)
#define SUCCEEDED(hr) (static_cast<HRESULT>(hr) >= 0)
#define IID_PPV_ARGS(pp) (pp)

std::vector<std::string> g_logs;
#define LOG_INFO(...) g_logs.push_back("INFO " + std::format(__VA_ARGS__))
#define LOG_WARN(...) g_logs.push_back("WARN " + std::format(__VA_ARGS__))
#define LOG_ERROR(...) g_logs.push_back("ERROR " + std::format(__VA_ARGS__))

unsigned Logged(const char* part)
{
    unsigned count = 0;
    for (const auto& line : g_logs)
        count += line.find(part) != std::string::npos;
    return count;
}

struct FakeEvent
{
    bool set = false;
    unsigned closes = 0;
};
using HANDLE = FakeEvent*;

struct ID3D12Fence
{
    uint64_t completed = 0;
    bool removed = false;
    unsigned releases = 0;
    HRESULT registerResult = S_OK;
    std::function<void()> onRegister; // what happens as the registration is asked for
    struct Registration
    {
        uint64_t value;
        FakeEvent* event;
    };
    std::vector<Registration> registrations;

    UINT64 GetCompletedValue() { return removed ? UINT64_MAX : completed; }

    HRESULT SetEventOnCompletion(UINT64 value, HANDLE event)
    {
        if (onRegister)
            onRegister();
        if (FAILED(registerResult))
            return registerResult;
        if (removed || completed >= value)
            event->set = true;
        else
            registrations.push_back({ value, event });
        return S_OK;
    }

    void Fire()
    {
        for (size_t i = 0; i < registrations.size();)
        {
            if (removed || completed >= registrations[i].value)
            {
                registrations[i].event->set = true;
                registrations.erase(registrations.begin() + static_cast<std::ptrdiff_t>(i));
            }
            else
            {
                ++i;
            }
        }
    }

    // The queue reached the value.
    void Reach(uint64_t value)
    {
        completed = value;
        Fire();
    }

    // The device was removed: every reader sees UINT64_MAX and every waiter is woken, with nothing having run.
    void Remove()
    {
        removed = true;
        Fire();
    }

    void Release() { ++releases; }
};

struct ID3D12CommandAllocator
{
    unsigned resets = 0;
    unsigned releases = 0;
    HRESULT Reset()
    {
        ++resets;
        return S_OK;
    }
    void SetName(const wchar_t*) {}
    void Release() { ++releases; }
};

struct ID3D12CommandList
{
};

struct ID3D12GraphicsCommandList : ID3D12CommandList
{
    unsigned resets = 0;
    unsigned closes = 0;
    unsigned releases = 0;
    HRESULT Reset(ID3D12CommandAllocator*, void*)
    {
        ++resets;
        return S_OK;
    }
    HRESULT Close()
    {
        ++closes;
        return S_OK;
    }
    void SetName(const wchar_t*) {}
    void Release() { ++releases; }
};

struct ID3D12Resource
{
    unsigned releases = 0;
    void Release() { ++releases; }
};

struct ID3D12CommandQueue
{
    unsigned executed = 0;
    unsigned signals = 0;
    HRESULT signalResult = S_OK;
    struct Owed
    {
        ID3D12Fence* fence;
        uint64_t value;
    };
    std::vector<Owed> owed;

    void ExecuteCommandLists(UINT count, ID3D12CommandList**) { executed += count; }

    HRESULT Signal(ID3D12Fence* fence, UINT64 value)
    {
        ++signals;
        if (FAILED(signalResult))
            return signalResult;
        owed.push_back({ fence, value });
        return S_OK;
    }

    // The GPU finishes everything submitted so far.
    void Drain()
    {
        for (const auto& signal : owed)
            signal.fence->Reach(signal.value);
        owed.clear();
    }
};

// Owns every fake, so an object the helpers abandon is not a leak the sanitizer reports.
struct World
{
    std::vector<std::unique_ptr<ID3D12CommandAllocator>> allocators;
    std::vector<std::unique_ptr<ID3D12GraphicsCommandList>> lists;
    std::vector<std::unique_ptr<ID3D12Fence>> fences;
    std::vector<std::unique_ptr<FakeEvent>> events;
    bool failFence = false;

    unsigned Releases() const
    {
        unsigned count = 0;
        for (const auto& allocator : allocators)
            count += allocator->releases;
        for (const auto& list : lists)
            count += list->releases;
        for (const auto& fence : fences)
            count += fence->releases;
        for (const auto& event : events)
            count += event->closes;
        return count;
    }
} g_world;

struct ID3D12Device
{
    HRESULT CreateCommandAllocator(int, ID3D12CommandAllocator** out)
    {
        g_world.allocators.push_back(std::make_unique<ID3D12CommandAllocator>());
        *out = g_world.allocators.back().get();
        return S_OK;
    }
    HRESULT CreateCommandList(UINT, int, ID3D12CommandAllocator*, void*, ID3D12GraphicsCommandList** out)
    {
        g_world.lists.push_back(std::make_unique<ID3D12GraphicsCommandList>());
        *out = g_world.lists.back().get();
        return S_OK;
    }
    HRESULT CreateFence(UINT64, int, ID3D12Fence** out)
    {
        if (g_world.failFence)
            return E_FAIL;
        g_world.fences.push_back(std::make_unique<ID3D12Fence>());
        *out = g_world.fences.back().get();
        return S_OK;
    }
};

HANDLE CreateEventW(void*, int, int, void*)
{
    g_world.events.push_back(std::make_unique<FakeEvent>());
    return g_world.events.back().get();
}

void CloseHandle(HANDLE event) { ++event->closes; }

// An auto-reset event. g_duringWait is what the GPU, or the device, does while the thread is blocked.
unsigned g_waits = 0;
DWORD g_lastTimeout = 0;
std::function<void()> g_duringWait;

DWORD WaitForSingleObject(HANDLE event, DWORD timeoutMs)
{
    ++g_waits;
    g_lastTimeout = timeoutMs;
    if (!event->set && g_duringWait)
        g_duringWait();
    if (!event->set)
        return WAIT_TIMEOUT;
    event->set = false;
    return WAIT_OBJECT_0;
}

// --- the production helpers ---------------------------------------------------------------------------------------

#include "artifacts/present_types.inc"

PresentList g_presentList;
UINT64 g_presentFenceValue = 0;
std::vector<PresentHdrRetired> g_hdrRetired;

std::mutex g_nrMutex;
void EndUntrackedPresent(ID3D12GraphicsCommandList*, ID3D12CommandQueue*, ID3D12Fence*, uint64_t) {}
#include "artifacts/present_list_slice.inc"

// --- scenarios ----------------------------------------------------------------------------------------------------

ID3D12Device g_device;
ID3D12CommandQueue g_queue;

ID3D12GraphicsCommandList* Acquire(unsigned int slot) { return GetPresentCommandList(&g_device, slot); }

// Acquire and submit on a slot; the list must have been handed out.
void Submit(unsigned int slot)
{
    ID3D12GraphicsCommandList* list = Acquire(slot);
    assert(list != nullptr);
    assert(SubmitPresentList(&g_queue, slot));
}

ID3D12Fence& Fence() { return *g_presentList.fence; }
ID3D12CommandAllocator& Allocator(unsigned int slot) { return *g_presentList.allocator[slot]; }

// A live device: the slot is reused once the fence passed its value, with no wait when it already has.
void ReuseAfterCompletion()
{
    Submit(0);
    assert(g_presentList.fenceValue[0] == 1 && g_queue.executed == 1 && g_queue.signals == 1);
    assert(Allocator(0).resets == 1);
    g_queue.Drain();

    assert(Acquire(0) != nullptr);
    assert(Allocator(0).resets == 2 && g_waits == 0 && g_logs.empty());
    assert(g_world.Releases() == 0);
}

// Still running: the bounded wait, then the reuse once the GPU got there during it.
void WaitsThenReuses()
{
    Submit(0);
    g_duringWait = [] { g_queue.Drain(); };

    assert(Acquire(0) != nullptr);
    assert(g_waits == 1 && g_lastTimeout == 5000 && Allocator(0).resets == 2 && g_logs.empty());
}

// Still running after the wait: no list, no reset, said once.
void BusySlotIsSkipped()
{
    Submit(0);

    assert(Acquire(0) == nullptr);
    assert(g_waits == 1 && Allocator(0).resets == 1);
    assert(Logged("allocator wait returned 258") == 1 && Logged("still in use by GPU") == 1);

    assert(Acquire(0) == nullptr);
    assert(g_waits == 2 && Allocator(0).resets == 1 && Logged("still in use by GPU") == 1);
    assert(g_world.Releases() == 0);
}

// The device is removed with a list in flight. UINT64_MAX is not "the list finished": the allocator is not reset,
// no list is handed out on any slot again, nothing is released, and it is said once.
void RemovedBeforeReuse()
{
    Submit(0);
    Fence().Remove();

    assert(Acquire(0) == nullptr);
    assert(Allocator(0).resets == 1 && g_presentList.list[0]->resets == 1 && g_waits == 0);
    assert(Logged("removed") == 1);

    // A slot that never submitted has nothing to wait for, and is not started on a removed device either.
    assert(Acquire(1) == nullptr);
    assert(Allocator(1).resets == 0 && g_presentList.list[1]->resets == 0);
    assert(Acquire(0) == nullptr);
    assert(Logged("removed") == 1 && g_logs.size() == 1);
    assert(g_world.Releases() == 0);
}

// The removal arrives during the allocator wait and wakes it. The wake is not completion.
void RemovedDuringAllocatorWait()
{
    Submit(0);
    g_duringWait = [] { Fence().Remove(); };

    assert(Acquire(0) == nullptr);
    assert(g_waits == 1 && Allocator(0).resets == 1);
    assert(Logged("removed") == 1 && Logged("still in use by GPU") == 0);
    assert(g_world.Releases() == 0);
}

// The removal shows first as a registration that fails.
void RemovedAsRegistrationFailure()
{
    Submit(0);
    Fence().registerResult = DXGI_ERROR_DEVICE_REMOVED;
    Fence().onRegister = [] { Fence().removed = true; };

    assert(Acquire(0) == nullptr);
    assert(g_waits == 0 && Allocator(0).resets == 1 && Logged("removed") == 1);
    assert(g_world.Releases() == 0);
}

// WaitPresentListsIdle on a live device: what it always answered.
void IdleOnLiveDevice()
{
    assert(WaitPresentListsIdle(2000)); // no list was ever built
    assert(Acquire(0) != nullptr);
    assert(SubmitPresentList(&g_queue, 0));
    g_queue.Drain();
    assert(WaitPresentListsIdle(2000) && g_waits == 0); // finished already: no wait

    Submit(1);
    g_duringWait = [] { g_queue.Drain(); };
    assert(WaitPresentListsIdle(2000) && g_waits == 1 && g_lastTimeout == 2000); // finished during the wait

    g_duringWait = nullptr;
    Submit(2);
    assert(!WaitPresentListsIdle(2000) && g_waits == 2); // a timeout keeps what the lists read

    Fence().registerResult = E_FAIL;
    assert(!WaitPresentListsIdle(2000) && g_waits == 2); // no registration, no proof
    assert(g_logs.empty() && g_world.Releases() == 0);
}

// Nothing was submitted: there is nothing in flight to keep anything for, whatever happened to the device.
void IdleWithNothingSubmitted()
{
    assert(Acquire(0) != nullptr);
    assert(WaitPresentListsIdle(2000) && g_waits == 0);
}

// A removed device never reads as idle: what the lists read is kept, not released.
void RemovedIsNotIdle()
{
    Submit(0);
    Fence().Remove();

    assert(!WaitPresentListsIdle(2000));
    assert(!WaitPresentListsIdle(2000));
    assert(g_waits == 0 && Logged("removed") == 1);
}

// The removal wakes the idle wait: woken is not finished.
void RemovedDuringIdleWait()
{
    Submit(0);
    g_duringWait = [] { Fence().Remove(); };

    assert(!WaitPresentListsIdle(2000));
    assert(g_waits == 1 && Logged("removed") == 1);
}

// The shared event was left set by an older registration (a wait that timed out, whose value arrived later). The
// wait returns at once for a value the fence has not reached.
void StaleEventIsNotIdle()
{
    Submit(0);
    g_presentList.fenceEvent->set = true;

    assert(!WaitPresentListsIdle(2000));
    assert(g_waits == 1 && g_logs.empty());

    g_queue.Drain();
    assert(WaitPresentListsIdle(2000));
}

// The HDR textures retired against the present fence: released at the value they were retired at, not before.
void HdrRetiredOnLiveDevice()
{
    Submit(0);
    Submit(1);
    Submit(2);
    assert(g_presentFenceValue == 3);

    ID3D12Resource texture;
    ID3D12Resource* retired = &texture;
    RetirePresentHdrTexture(retired);
    assert(retired == nullptr && g_hdrRetired.size() == 1 && g_hdrRetired[0].fenceValue == 3);

    Fence().Reach(2);
    CollectPresentHdrRetired();
    assert(texture.releases == 0 && g_hdrRetired.size() == 1);

    g_queue.Drain();
    CollectPresentHdrRetired();
    assert(texture.releases == 1 && g_hdrRetired.empty() && g_logs.empty());
}

// The review's minimal case: retired at 3, the device is removed, the fence reads UINT64_MAX. Kept, for good.
void HdrRetiredOnRemovedDevice()
{
    Submit(0);
    Submit(1);
    Submit(2);

    ID3D12Resource texture;
    ID3D12Resource* retired = &texture;
    RetirePresentHdrTexture(retired);
    Fence().Reach(2);
    Fence().Remove();

    for (int i = 0; i < 3; ++i)
        CollectPresentHdrRetired();
    assert(texture.releases == 0 && g_hdrRetired.size() == 1 && g_hdrRetired[0].resource == &texture);
    assert(Logged("removed") == 1 && g_world.Releases() == 0);
}

// Signal fails because the device is gone, before the fence says so. The list was handed to the queue, so what was
// recorded on it is owed "executed"; nothing is proven about it, so the route ends as on any removal.
void SignalFailsOnRemoval()
{
    assert(Acquire(0) != nullptr);
    g_queue.signalResult = DXGI_ERROR_DEVICE_REMOVED;
    assert(SubmitPresentList(&g_queue, 0));
    assert(g_queue.executed == 1 && !g_presentList.dirty[0] && Logged("removed") == 1);

    assert(Acquire(1) == nullptr);
    assert(Acquire(0) == nullptr);
    assert(Allocator(0).resets == 1 && Allocator(1).resets == 0 && g_waits == 0);
    assert(!WaitPresentListsIdle(2000) && g_waits == 0);
    assert(Logged("removed") == 1 && g_world.Releases() == 0);
}

// Signal fails on a live device. Executed all the same, said once, and the slot stays in use until a later signal
// on the queue passes the value this one never reached.
void SignalFailsOnLiveDevice()
{
    assert(Acquire(0) != nullptr);
    g_queue.signalResult = E_FAIL;
    assert(SubmitPresentList(&g_queue, 0));
    assert(g_presentList.fenceValue[0] == 1 && Logged("signal failed") == 1 && Logged("removed") == 0);

    assert(Acquire(1) != nullptr);
    assert(SubmitPresentList(&g_queue, 1));
    assert(Logged("signal failed") == 1); // once

    g_queue.signalResult = S_OK;
    Submit(2);
    assert(g_presentList.fenceValue[2] == 3);

    assert(Acquire(0) == nullptr); // value 1 was never signalled and nothing later has finished
    assert(Allocator(0).resets == 1 && g_waits == 1);

    g_queue.Drain(); // 3 reached: the lists before it on this queue have finished too
    assert(Acquire(0) != nullptr);
    assert(Allocator(0).resets == 2 && g_world.Releases() == 0);
}

// A build that fails part-way releases what it made: nothing was ever submitted on it.
void FailedBuildReleasesUnused()
{
    g_world.failFence = true;
    assert(Acquire(0) == nullptr);
    assert(g_world.allocators.size() == PresentList::kSlots && g_world.lists.size() == PresentList::kSlots);
    for (const auto& allocator : g_world.allocators)
        assert(allocator->releases == 1);
    for (const auto& list : g_world.lists)
        assert(list->releases == 1);
    assert(g_presentList.list[0] == nullptr && g_presentList.fence == nullptr);
}

const struct
{
    const char* name;
    void (*run)();
} kScenarios[] = {
    { "reuse-after-completion", ReuseAfterCompletion },
    { "waits-then-reuses", WaitsThenReuses },
    { "busy-slot-is-skipped", BusySlotIsSkipped },
    { "removed-before-reuse", RemovedBeforeReuse },
    { "removed-during-allocator-wait", RemovedDuringAllocatorWait },
    { "removed-as-registration-failure", RemovedAsRegistrationFailure },
    { "idle-on-live-device", IdleOnLiveDevice },
    { "idle-with-nothing-submitted", IdleWithNothingSubmitted },
    { "removed-is-not-idle", RemovedIsNotIdle },
    { "removed-during-idle-wait", RemovedDuringIdleWait },
    { "stale-event-is-not-idle", StaleEventIsNotIdle },
    { "hdr-retired-on-live-device", HdrRetiredOnLiveDevice },
    { "hdr-retired-on-removed-device", HdrRetiredOnRemovedDevice },
    { "signal-fails-on-removal", SignalFailsOnRemoval },
    { "signal-fails-on-live-device", SignalFailsOnLiveDevice },
    { "failed-build-releases-unused", FailedBuildReleasesUnused },
};
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0)
    {
        for (const auto& scenario : kScenarios)
            std::printf("%s\n", scenario.name);
        return 0;
    }

    for (const auto& scenario : kScenarios)
    {
        if (argc == 2 && std::strcmp(argv[1], scenario.name) == 0)
        {
            scenario.run();
            return 0;
        }
    }

    std::fprintf(stderr, "usage: cases --list | <scenario>\n");
    return 2;
}
