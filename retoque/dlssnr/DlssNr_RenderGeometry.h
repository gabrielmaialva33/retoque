#pragma once

#include "DlssNr_RenderSize.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

// D3D12 NGX handle lifetime and evaluate binding. See design/render-geometry.md.
namespace DlssNr::RenderGeometry
{
struct Creation
{
    RenderSize render, display;
};
inline Creation ReadCreation(const NVSDK_NGX_Parameter* params)
{
    // Snapshot creation Width/Height, not a backend's mutable RenderWidth or evaluate subrect.
    Creation result;
    if (params)
    {
        auto* p = const_cast<NVSDK_NGX_Parameter*>(params);
        p->Get(NVSDK_NGX_Parameter_Width, &result.render.width);
        p->Get(NVSDK_NGX_Parameter_Height, &result.render.height);
        result.display = ReadDisplaySize(params);
    }
    return result;
}
struct Record
{
    Creation creation;
    std::atomic_bool stale = false;
    explicit Record(Creation value) : creation(value) {}
};
inline std::mutex recordsMutex;
inline std::unordered_map<uint64_t, std::shared_ptr<Record>> records;
inline void Remember(uint64_t handle, Creation creation)
{
    std::lock_guard lock(recordsMutex);
    records.erase(handle);
    if (creation.render.width && creation.render.height && creation.display.width && creation.display.height &&
        creation.render.width <= creation.display.width && creation.render.height <= creation.display.height)
        records.emplace(handle, std::make_shared<Record>(creation));
}
inline void Forget(uint64_t handle)
{
    std::lock_guard lock(recordsMutex);
    records.erase(handle);
}
class Scope
{
    std::shared_ptr<Record> record;
    Binding binding;
    const Binding* previous;

    void Observe() const
    {
        if (!record)
            return;
        const auto current = ReadRenderSize(binding.params);
        const auto& initial = record->creation;
        const auto display = ReadDisplaySize(binding.params);
        if ((current.size.width && current.size.width != initial.render.width) ||
            (current.size.height && current.size.height != initial.render.height) ||
            (display.width && display.width != initial.display.width) ||
            (display.height && display.height != initial.display.height))
            record->stale.store(true, std::memory_order_relaxed);
    }

  public:
    Scope(bool enabled, uint64_t handle, const NVSDK_NGX_Parameter* params) : previous(active)
    {
        // Even a disabled nested call must not inherit an outer evaluate's binding.
        active = nullptr;
        if (!enabled || !params)
            return;
        {
            std::lock_guard lock(recordsMutex);
            const auto it = records.find(handle);
            if (it != records.end())
                record = it->second;
        }
        if (!record)
            return;
        binding = { params, record->creation.render, record->creation.display, &record->stale };
        Observe();
        active = &binding;
    }
    ~Scope()
    {
        // Backends may normalize dimensions in-place during the evaluate. Never re-arm an epoch.
        Observe();
        active = previous;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
} // namespace DlssNr::RenderGeometry
