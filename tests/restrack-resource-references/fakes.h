// Host fakes for the D3D12 resource/descriptor tracking slices of ResTrack_dx12.{h,cpp}.
// Only the types and macros the sliced code names are provided; no D3D12 runtime is involved.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

#if __has_include(<resource_tracking/ResourceReferenceIndex.h>)
#include <resource_tracking/ResourceReferenceIndex.h>
#define RESTRACK_HAS_REFERENCE_INDEX 1
#else
#define RESTRACK_HAS_REFERENCE_INDEX 0
#endif

#define __stdcall
#define LOG_TRACK(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define FAILED(hr) ((hr) < 0)
#define IID_PPV_ARGS(pp) 0, reinterpret_cast<void**>(pp)

using UINT = unsigned int;
using UINT64 = std::uint64_t;
using SIZE_T = std::size_t;
using ULONG = unsigned long;
using HRESULT = long;

constexpr HRESULT E_NOINTERFACE_FAKE = -2147467262L;

enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
};

enum D3D12_RESOURCE_STATES
{
    D3D12_RESOURCE_STATE_COMMON = 0,
};

enum D3D12_RESOURCE_FLAGS
{
    D3D12_RESOURCE_FLAG_NONE = 0,
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET = 1,
};

enum ResourceType
{
    SRV,
    RTV,
    UAV
};

struct ID3D12DescriptorHeap
{
};

using PFN_DESTRUCTION_CALLBACK = void (*)(void*);

struct ID3D12Resource;

struct ID3DDestructionNotifier
{
    ID3D12Resource* owner = nullptr;
    std::atomic<int> references { 0 };

    HRESULT RegisterDestructionCallback(PFN_DESTRUCTION_CALLBACK callback, void* data, UINT* id);
    ULONG Release() { return static_cast<ULONG>(--references); }
};

// A resource whose destruction notifier records callbacks; Destroy() runs them as the runtime would.
struct ID3D12Resource
{
    bool notifierAvailable = true;
    HRESULT registerResult = 0;
    ID3DDestructionNotifier notifier;
    std::mutex callbackMutex;
    std::vector<std::pair<PFN_DESTRUCTION_CALLBACK, void*>> callbacks;

    ID3D12Resource() { notifier.owner = this; }

    HRESULT QueryInterface(int, void** out)
    {
        if (!notifierAvailable)
        {
            *out = nullptr;
            return E_NOINTERFACE_FAKE;
        }

        notifier.references++;
        *out = &notifier;
        return 0;
    }

    size_t CallbackCount()
    {
        std::scoped_lock lock(callbackMutex);
        return callbacks.size();
    }

    void Destroy()
    {
        std::vector<std::pair<PFN_DESTRUCTION_CALLBACK, void*>> run;
        {
            std::scoped_lock lock(callbackMutex);
            run.swap(callbacks);
        }

        for (auto& [callback, data] : run)
            callback(data);
    }
};

inline HRESULT ID3DDestructionNotifier::RegisterDestructionCallback(PFN_DESTRUCTION_CALLBACK callback, void* data,
                                                                    UINT* id)
{
    if (FAILED(owner->registerResult))
        return owner->registerResult;

    std::scoped_lock lock(owner->callbackMutex);
    owner->callbacks.emplace_back(callback, data);
    *id = static_cast<UINT>(owner->callbacks.size());
    return 0;
}

struct State
{
    bool isShuttingDown = false;
    static State& Instance()
    {
        static State state;
        return state;
    }
};

struct Hudfix_Dx12
{
    static inline std::mutex removedMutex;
    static inline std::vector<ID3D12Resource*> removed;

    static void RemoveResourceFromTracking(ID3D12Resource* resource)
    {
        std::scoped_lock lock(removedMutex);
        removed.push_back(resource);
    }
};

class ResTrack_Dx12
{
  public:
    static void __stdcall ResourceDestroyed(void* data);
    static bool TrackResourceRelease(ID3D12Resource* resource);
};
