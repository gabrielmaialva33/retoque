#include "pch.h"

#include "DlssNr_Submission.h"
#include <resource_tracking/ResTrack_dx12.h>
#include <Logger.h>
#include <detours/detours.h>
#include <atomic>
#include <mutex>
#include <new>

namespace DlssNr::Submission
{
namespace
{
std::atomic<bool> active { false };
std::atomic<const char*> refusal { "not-enabled" };
std::mutex stateMutex;
std::mutex installMutex;
std::mutex submitMutex;
Detail::Model model;

struct Queue
{
    ID3D12CommandQueue* object = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t next = 0;
};
// Bounded process-lifetime references intentionally survive device replacement.
// No device-lost result can authorize destruction of retained renderer objects.
std::array<Queue, Detail::MaxQueues> queues {};

using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
ResetFn originalReset = nullptr;
void* resetTarget = nullptr;
uintptr_t admittedDevice = 0;
ID3D12Device* deviceOwner = nullptr;

HMODULE NativeRuntimeModule(void* method)
{
    HMODULE module = nullptr;
    if (!method ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(method), &module))
        return nullptr;
    wchar_t path[MAX_PATH] {};
    const auto length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
        return nullptr;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return (_wcsicmp(name, L"d3d12.dll") == 0 || _wcsicmp(name, L"d3d12core.dll") == 0) ? module : nullptr;
}

template <typename T> T* Canonical(T* object)
{
    auto real = ResTrack_Dx12::NrRealObject(object);
    T* typed = nullptr;
    if (!real || FAILED(real->QueryInterface(IID_PPV_ARGS(&typed))))
        return nullptr;
    typed->Release(); // Original object's caller/reference keeps this identity alive.
    return typed;
}

uintptr_t Key(IUnknown* object)
{
    auto real = ResTrack_Dx12::NrRealObject(object);
    IUnknown* identity = nullptr;
    if (!real || FAILED(real->QueryInterface(IID_PPV_ARGS(&identity))))
        return 0;
    const auto key = reinterpret_cast<uintptr_t>(identity);
    identity->Release();
    return key;
}

uint64_t FenceValue(size_t queue)
{
    return queues[queue].fence ? queues[queue].fence->GetCompletedValue() : UINT64_MAX;
}

HRESULT STDMETHODCALLTYPE ResetHook(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator,
                                    ID3D12PipelineState* initialState)
{
    std::shared_ptr<Detail::Epoch> epoch;
    if (active.load(std::memory_order_acquire))
    {
        auto real = Canonical(list);
        std::lock_guard lock(stateMutex);
        epoch = model.Current(Key(real));
    }
    const auto result = originalReset(list, allocator, initialState);
    if (epoch)
    {
        std::lock_guard lock(stateMutex);
        Detail::Model::AfterReset(epoch, SUCCEEDED(result));
    }
    return result;
}

bool Install(ID3D12GraphicsCommandList* list)
{
    std::lock_guard lock(installMutex);
    void* target = (*reinterpret_cast<void***>(list))[10]; // ID3D12GraphicsCommandList::Reset
    if (originalReset && target != resetTarget)
    {
        refusal = "unsupported-reset-implementation";
        return false;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
    {
        refusal = "missing-device";
        return false;
    }
    const bool queueHook = ResTrack_Dx12::EnableNrSubmissionTracking(device);
    if (!queueHook)
    {
        device->Release();
        refusal = "queue-hook-unavailable";
        return false;
    }
    const auto deviceKey = Key(device);
    const auto runtime = NativeRuntimeModule(target);
    if (!deviceKey || (admittedDevice && admittedDevice != deviceKey) || !runtime ||
        NativeRuntimeModule(ResTrack_Dx12::NrQueueImplementation()) != runtime)
    {
        device->Release();
        refusal = "unsupported-runtime-or-device";
        return false;
    }
    if (!originalReset)
    {
        originalReset = reinterpret_cast<ResetFn>(target);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        const auto attached = DetourAttach(reinterpret_cast<PVOID*>(&originalReset), ResetHook);
        const auto committed = DetourTransactionCommit();
        if (attached != NO_ERROR || committed != NO_ERROR)
        {
            device->Release();
            originalReset = nullptr;
            refusal = "reset-hook-unavailable";
            return false;
        }
        resetTarget = target;
    }
    if (!deviceOwner)
    {
        admittedDevice = deviceKey;
        deviceOwner = device;
    }
    else
        device->Release();
    active.store(true, std::memory_order_release);
    return true;
}

size_t FindQueue(ID3D12CommandQueue* queue)
{
    queue = Canonical(queue);
    if (!queue)
        return Detail::MaxQueues;
    for (size_t i = 0; i < queues.size(); ++i)
        if (queues[i].object == queue)
            return i;
    for (size_t i = 0; i < queues.size(); ++i)
    {
        auto& q = queues[i];
        if (q.object)
            continue;
        ID3D12Device* device = nullptr;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
            return Detail::MaxQueues;
        if (Key(device) != admittedDevice ||
            (*reinterpret_cast<void***>(queue))[10] != ResTrack_Dx12::NrQueueImplementation())
        {
            device->Release();
            return Detail::MaxQueues;
        }
        ID3D12Fence* fence = nullptr;
        const auto result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        device->Release();
        if (FAILED(result))
            return Detail::MaxQueues;
        queue->AddRef();
        q.object = queue;
        q.fence = fence;
        return i;
    }
    return Detail::MaxQueues;
}
} // namespace

bool Active() { return active.load(std::memory_order_acquire); }
const char* LastRefusal() { return refusal.load(); }

bool Track(ID3D12GraphicsCommandList* list, Usage& usage, bool chain)
{
    if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        refusal = "unsupported-command-list-type";
        return false;
    }
    list = Canonical(list);
    if (!list || !Install(list))
        return false;
    std::lock_guard lock(stateMutex);
    try
    {
        auto epoch = model.Track(Key(list), usage, FenceValue);
        if (!epoch)
        {
            refusal = "recording-capacity-exhausted";
            return false;
        }
        epoch->chain |= chain;
        if (!epoch->owner)
        {
            list->AddRef();
            epoch->owner =
                std::shared_ptr<void>(list, [](void* p) { static_cast<ID3D12GraphicsCommandList*>(p)->Release(); });
        }
        return true;
    }
    catch (const std::bad_alloc&)
    {
        refusal = "recording-allocation-failed";
        return false;
    }
}

bool Ready(const Usage& usage)
{
    std::lock_guard lock(stateMutex);
    for (const auto& e : usage.epochs)
        if (e && !Detail::IsReady(*e, FenceValue))
            return false;
    return true;
}

void Advance(uint64_t now)
{
    std::lock_guard lock(stateMutex);
    model.Advance(now);
}

uint64_t Clock()
{
    std::lock_guard lock(stateMutex);
    return model.Clock();
}

bool Strict()
{
    std::lock_guard lock(stateMutex);
    return model.Strict();
}

bool MultiQueue()
{
    std::lock_guard lock(stateMutex);
    return model.MultiQueue();
}

namespace
{
// Under stateMutex. A list whose only remaining reference is ours was released by the game: nobody can execute it
// again. Our reference keeps the identity alive, so nothing can take the count back up; once set it stays set.
void RefreshOrphans(const Usage& usage)
{
    for (const auto& e : usage.epochs)
    {
        if (!e || e->orphaned || !e->owner)
            continue;
        auto* list = static_cast<ID3D12GraphicsCommandList*>(e->owner.get());
        list->AddRef();
        e->orphaned = list->Release() == 1;
    }
}
} // namespace

Detail::Admission Admit(Usage& usage, ID3D12GraphicsCommandList* list)
{
    const auto key = list ? Key(list) : 0;
    std::lock_guard lock(stateMutex);
    RefreshOrphans(usage);
    Detail::PruneLifetime(usage, FenceValue, model.Clock(), model.Strict());
    return Detail::Admit(usage, FenceValue, model.Clock(), model.Strict(), model.Overlap(), key);
}

bool Settled(const Usage& usage)
{
    std::lock_guard lock(stateMutex);
    RefreshOrphans(usage);
    return Detail::IsSettled(usage, FenceValue, model.Clock(), model.Strict());
}

bool OnlyUnanswerableLeft(const Usage& usage)
{
    std::lock_guard lock(stateMutex);
    RefreshOrphans(usage);
    return Detail::IsOnlyUnanswerableLeft(usage, FenceValue, model.Clock(), model.Strict());
}

void PruneFinished(Usage& usage)
{
    // No RefreshOrphans: an orphan flag only lets an epoch finish earlier, and Settled refreshes the flags of the
    // shared epochs anyway. Refreshing here cost a list AddRef/Release per epoch per copied candidate per sweep.
    std::lock_guard lock(stateMutex);
    Detail::PruneFinished(usage, FenceValue, model.Clock(), model.Strict());
}

void Prune(Usage& usage)
{
    std::lock_guard lock(stateMutex);
    for (auto& e : usage.epochs)
        if (e && Detail::IsReady(*e, FenceValue))
            e.reset();
}

bool Completed(const Usage& usage)
{
    std::lock_guard lock(stateMutex);
    bool any = false;
    for (const auto& e : usage.epochs)
    {
        if (!e)
            continue;
        any = true;
        if (!Detail::IsComplete(*e, FenceValue))
            return false;
    }
    return any;
}

bool SubmittedOnlyOn(const Usage& usage, ID3D12CommandQueue* queue)
{
    if (queue == nullptr)
        return false;
    std::lock_guard lock(stateMutex);
    const size_t qIndex = FindQueue(queue);
    if (qIndex >= queues.size())
        return false;

    bool any = false;
    for (const auto& e : usage.epochs)
    {
        if (!e)
            continue;
        any = true;
        if (!Detail::IsSubmittedOnlyOn(*e, qIndex))
            return false;
    }
    return any;
}

void Abandon(ID3D12GraphicsCommandList* list)
{
    if (list == nullptr)
        return;
    auto real = Canonical(list);
    std::lock_guard lock(stateMutex);
    model.Abandon(Key(real));
}

GpuTiming::Detail::Certificate TimingCertificate(const Usage& usage)
{
    std::lock_guard lock(stateMutex);
    const auto frequency = [](size_t queue) -> uint64_t
    {
        UINT64 value = 0;
        return queues[queue].object && SUCCEEDED(queues[queue].object->GetTimestampFrequency(&value)) ? value : 0;
    };
    return GpuTiming::Detail::CertifyUsage(usage, FenceValue, frequency);
}

struct Batch::Impl
{
    std::unique_lock<std::mutex> serial { submitMutex };
    std::array<std::shared_ptr<Detail::Epoch>, Detail::MaxEpochs> epochs {};
    size_t count = 0;
    size_t queue = Detail::MaxQueues;
    bool notified = false;
};

Batch::Batch(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (!Active() || !lists)
        return;
    // Avoid allocating or acquiring the submit lock for unrelated queues/lists.
    bool tracked = false;
    {
        std::lock_guard lock(stateMutex);
        for (UINT i = 0; i < count && !tracked; ++i)
        {
            auto list = Canonical(lists[i]);
            tracked = bool(model.Current(Key(list)));
        }
    }
    if (!tracked)
        return;
    impl.reset(new (std::nothrow) Impl);
    if (!impl)
    {
        // No notification is possible. Poison each affected epoch BEFORE execution.
        std::lock_guard lock(stateMutex);
        for (UINT i = 0; i < count; ++i)
            if (auto e = model.Current(Key(lists[i])))
                e->unknown = true;
        refusal = "submission-allocation-failed";
        return;
    }
    {
        std::lock_guard lock(stateMutex);
        impl->queue = FindQueue(queue);
        for (UINT i = 0; i < count; ++i)
        {
            auto key = Key(lists[i]);
            auto e = model.Current(key);
            if (!e)
                continue;
            bool duplicate = false;
            for (size_t j = 0; j < impl->count; ++j)
                duplicate |= impl->epochs[j] == e;
            if (!duplicate)
                impl->epochs[impl->count++] = model.BeforeExecute(key);
            else
                model.DuplicateInBatch(e); // One batch can contain the same recording more than once.
        }
        // Tracked work moving to another queue (docs/dlssnr/design/chain-in-flight.md). Recordings were allowed
        // to overlap only after AffinityBatches on one queue, so this one may overlap one still running on the
        // previous queue: nothing is injected to order them (a dependency only the game knows could make that a
        // cycle). It is logged, and admission is serialized for the rest of the session.
        bool chain = false;
        for (size_t j = 0; j < impl->count; ++j)
            chain |= impl->epochs[j] && impl->epochs[j]->chain;
        const size_t previous = chain ? model.NoteQueue(impl->queue) : Detail::MaxQueues;
        if (previous < queues.size() && queues[previous].fence && queues[previous].next != 0)
        {
            const uint64_t reached = queues[previous].fence->GetCompletedValue();
            static bool said = false;
            if (!said)
            {
                said = true;
                LOG_WARN("DLSS-NR submission: tracked work moved to another queue ({}); NR recordings are "
                         "serialized from now on",
                         reached == UINT64_MAX              ? "the device was removed"
                         : reached >= queues[previous].next ? "the previous queue had finished"
                                                            : "the previous queue had not finished: one recording "
                                                              "may overlap it");
            }
        }
    }
}

void Batch::Submitted()
{
    if (!impl || impl->notified)
        return;
    bool success = false;
    uint64_t value = 0;
    if (impl->queue < queues.size())
    {
        auto& q = queues[impl->queue];
        if (q.next < UINT64_MAX - 1 && q.fence->GetCompletedValue() != UINT64_MAX)
        {
            value = ++q.next;
            // Queue Signal is ordered AFTER the original ExecuteCommandLists.
            success = SUCCEEDED(q.object->Signal(q.fence, value));
        }
    }
    {
        std::lock_guard lock(stateMutex);
        for (size_t i = 0; i < impl->count; ++i)
            Detail::Model::AfterExecute(impl->epochs[i], impl->queue, value, success);
    }
    if (!success)
    {
        refusal = "submission-fence-unavailable";
        LOG_WARN("DLSS-NR submission fence unavailable; tracked resources retained");
    }
    impl->notified = true;
    impl->serial.unlock();
}

Batch::~Batch()
{
    if (impl && !impl->notified)
    {
        std::lock_guard lock(stateMutex);
        for (size_t i = 0; i < impl->count; ++i)
            Detail::Model::AfterExecute(impl->epochs[i], Detail::MaxQueues, 0, false);
    }
}
} // namespace DlssNr::Submission
