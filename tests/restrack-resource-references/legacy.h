// Reference model: the reverse-reference bookkeeping of ResTrack_dx12 before the sharded index
// (source be4d9cf2, OptiScaler/resource_tracking/ResTrack_dx12.h and .cpp). One global mutex, one map,
// and a vector per resource scanned with find_if/remove_if. Bodies keep the original statements; only the
// globals became members and the D3D12 types became template parameters.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <ankerl/unordered_dense.h>

template <class Resource, class Heap> class LegacyReferences
{
  public:
    struct TrackedResourceSlot
    {
        std::weak_ptr<Heap> heap;
        uint64_t heapVersion = 0;
        unsigned index = 0;
    };

    ankerl::unordered_dense::map<Resource*, std::vector<TrackedResourceSlot>> _trackedResources;
    std::mutex _trackedResourcesMutex;

    // TrackResourceRelease: _trackedResources.contains(resource)
    bool Contains(Resource* resource)
    {
        std::lock_guard trackedLock(_trackedResourcesMutex);
        return _trackedResources.contains(resource);
    }

    // TrackResourceRelease after registering the destruction callback
    void Register(Resource* resource)
    {
        std::lock_guard trackedLock(_trackedResourcesMutex);
        _trackedResources.try_emplace(resource);
    }

    // HeapInfo::AttachToNewResourceLocked; false where the original nulled info[index].buffer
    template <class MakeHeap>
    bool Attach(Resource* newResource, uint64_t currentVersion, unsigned index, MakeHeap&& make)
    {
        std::scoped_lock lock(_trackedResourcesMutex);
        auto it = _trackedResources.find(newResource);
        if (it == _trackedResources.end())
            return false;

        auto& vec = it->second;
        auto found = std::find_if(vec.begin(), vec.end(), [currentVersion, index](const TrackedResourceSlot& slot)
                                  { return slot.heapVersion == currentVersion && slot.index == index; });

        if (found == vec.end())
            vec.push_back({ make(), currentVersion, index });

        return true;
    }

    // HeapInfo::DetachFromOldResourceLocked and the per-slot body of DeactivateAndClear
    void Detach(Resource* oldResource, uint64_t currentVersion, unsigned index)
    {
        std::scoped_lock lock(_trackedResourcesMutex);
        auto it = _trackedResources.find(oldResource);
        if (it == _trackedResources.end())
            return;

        auto& vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(), [currentVersion, index](const TrackedResourceSlot& slot)
                                 { return slot.heapVersion == currentVersion && slot.index == index; }),
                  vec.end());
    }

    // ResTrack_Dx12::ResourceDestroyed
    std::vector<TrackedResourceSlot> Take(Resource* resource)
    {
        std::vector<TrackedResourceSlot> toClean;
        std::lock_guard lock(_trackedResourcesMutex);
        if (auto it = _trackedResources.find(resource); it != _trackedResources.end())
        {
            toClean = std::move(it->second);
            _trackedResources.erase(it);
        }
        return toClean;
    }
};
