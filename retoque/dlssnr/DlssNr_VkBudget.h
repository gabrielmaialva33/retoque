#pragma once

#include "DlssNr_VkChain.h"
#include <vulkan/vulkan.h>
#include <chrono>
#include <span>

namespace DlssNr::VkChain
{
// Only the requested device opts in; defaults keep the application's extension list untouched.
inline bool EnableBudgetExtension(bool requested, bool properties2, const std::vector<std::string>& asked,
                                  const std::vector<std::string>& supported, std::vector<std::string>& add)
{
    if (!requested || !properties2 ||
        std::find(supported.begin(), supported.end(), VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == supported.end())
        return false;
    if (std::find(asked.begin(), asked.end(), VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == asked.end() &&
        std::find(add.begin(), add.end(), VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == add.end())
        add.emplace_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    return true;
}
enum class BudgetReason
{
    None,
    Extension,
    Query,
    Stale,
    HeapShape,
    Attribution,
    Overflow,
    Headroom,
    Retained,
};
inline const char* BudgetText(BudgetReason reason)
{
    switch (reason)
    {
    case BudgetReason::None:
        return "ready";
    case BudgetReason::Extension:
        return "memory-budget extension unavailable";
    case BudgetReason::Query:
        return "memory-budget query unavailable or invalid";
    case BudgetReason::Stale:
        return "memory-budget query stale";
    case BudgetReason::HeapShape:
        return "exactly one device-local heap required";
    case BudgetReason::Attribution:
        return "texture memory attribution unknown or mixed";
    case BudgetReason::Overflow:
        return "texture reservation overflow";
    case BudgetReason::Headroom:
        return "insufficient device-local headroom";
    case BudgetReason::Retained:
        return "retained generation limit";
    }
    return "unknown budget refusal";
}
inline uint64_t BudgetNowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
struct BudgetSample
{
    BudgetReason reason = BudgetReason::Query;
    VkPhysicalDeviceMemoryProperties memory {};
    uint32_t heap = UINT32_MAX;
    uint64_t budget = 0, used = 0, at = 0;
};
// Immutable device association after creation. There is no cached sample or GPU allocation.
// vkGetPhysicalDeviceMemoryProperties2 has no VkResult: unwritten/invalid output fails closed.
class BudgetSource
{
    VkPhysicalDevice _physical = VK_NULL_HANDLE;
    PFN_vkGetPhysicalDeviceMemoryProperties2 _query = nullptr;
    bool _enabled = false;
    const std::atomic<bool>* _lost = nullptr;
    uint64_t _generation = 0;
    inline static std::atomic<uint64_t> _nextGeneration { 1 };
    uint64_t (*_clock)() = BudgetNowMs;

  public:
    static constexpr uint64_t MaxAgeMs = 100;
    void Configure(VkPhysicalDevice physical, bool enabledAndSupported, PFN_vkGetPhysicalDeviceMemoryProperties2 query,
                   uint64_t (*clock)() = BudgetNowMs, const std::atomic<bool>* lost = nullptr)
    {
        auto identity = _nextGeneration.load(std::memory_order_relaxed);
        while (identity && !_nextGeneration.compare_exchange_weak(identity, identity == UINT64_MAX ? 0 : identity + 1,
                                                                  std::memory_order_relaxed))
        {
        }
        _generation = identity;
        _lost = lost;
        _physical = physical;
        _enabled = enabledAndSupported;
        _query = query;
        _clock = clock;
    }
    uint64_t Generation() const { return _generation; }
    bool Fresh(const BudgetSample& sample) const
    {
        const auto now = _clock();
        return (!_lost || !_lost->load(std::memory_order_acquire)) && now >= sample.at && now - sample.at <= MaxAgeMs;
    }
    BudgetSample Read() const
    {
        BudgetSample out;
        if (!_enabled)
        {
            out.reason = BudgetReason::Extension;
            return out;
        }
        if (!_generation || !_physical || !_query || (_lost && _lost->load(std::memory_order_acquire)))
            return out;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budgets {};
        budgets.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        for (uint32_t i = 0; i != VK_MAX_MEMORY_HEAPS; ++i)
            budgets.heapBudget[i] = budgets.heapUsage[i] = UINT64_MAX;
        VkPhysicalDeviceMemoryProperties2 properties {};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        properties.pNext = &budgets;
        out.at = _clock();
        _query(_physical, &properties);
        if (!Fresh(out))
        {
            out.reason = BudgetReason::Stale;
            return out;
        }
        out.memory = properties.memoryProperties;
        if (!out.memory.memoryHeapCount || out.memory.memoryHeapCount > VK_MAX_MEMORY_HEAPS ||
            !out.memory.memoryTypeCount || out.memory.memoryTypeCount > VK_MAX_MEMORY_TYPES)
            return out;
        for (uint32_t i = 0; i != out.memory.memoryHeapCount; ++i)
        {
            if (!(out.memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
                continue;
            if (out.heap != UINT32_MAX)
            {
                out.reason = BudgetReason::HeapShape;
                return out;
            }
            out.heap = i;
        }
        if (out.heap == UINT32_MAX)
        {
            out.reason = BudgetReason::HeapShape;
            return out;
        }
        out.budget = budgets.heapBudget[out.heap];
        out.used = budgets.heapUsage[out.heap];
        if (!out.budget || out.budget == UINT64_MAX || out.used == UINT64_MAX ||
            out.budget > out.memory.memoryHeaps[out.heap].size)
            return out;
        out.reason = BudgetReason::None;
        return out;
    }
};
struct TextureReservation
{
    uint32_t memoryType = UINT32_MAX;
    uint64_t bytes = 0; // actual VkMemoryRequirements::size or a larger proved reservation, never pixels * bpp
};
// Meaningful changes only: no frame/time/budget/usage/recording epoch here. Device identity is
// a non-recycled generation identity, not a reusable numeric Vulkan handle.
struct BudgetKey
{
    uint64_t device = 0, deviceGeneration = 0;
    uint32_t width = 0, height = 0, workWidth = 0, workHeight = 0, format = 0, filter = 0, count = 1;
    bool individual = false;
    std::array<DlssNrResolvedPassSettings, PassConfig::MaxPasses> settings {};
    bool operator==(const BudgetKey&) const = default;
};
struct BudgetDecision
{
    BudgetReason reason = BudgetReason::None;
    bool log = false, legacy = false;
    BudgetSample sample {};
    BudgetKey key {};
    uint64_t textures = 0;
};
// Renderer mutex serializes this state. Refusal survives time, recording resets and budget
// fluctuations. A meaningful settings/device/geometry change is the only way to try again.
class BudgetGate
{
    std::optional<BudgetKey> _failed;
    BudgetReason _reason = BudgetReason::None;

  public:
    static BudgetKey Canonical(BudgetKey key)
    {
        key.count = PassConfig::BoundCount(key.count);
        key.individual = false; // only the resolved tuning changes work
        for (size_t i = key.count; i < key.settings.size(); ++i)
            key.settings[i] = {};
        return key;
    }
    bool Fail(BudgetKey key, BudgetReason reason)
    {
        key = Canonical(key);
        const bool changed = !_failed || *_failed != key;
        _failed = key;
        _reason = reason;
        return changed;
    }
    BudgetDecision Check(BudgetKey key, const BudgetSource& source, uint64_t model,
                         std::span<const TextureReservation> textures)
    {
        BudgetDecision out;
        if (key.count <= 1)
        {
            out.legacy = true;
            return out; // exactly no source call, latch mutation or allocation
        }
        key = Canonical(key);
        out.key = key;
        if (_failed && *_failed == key)
        {
            out.reason = _reason;
            return out;
        }
        _failed.reset();
        out.sample = source.Read(); // fresh on every admitted creation attempt; never reuse a previous sample
        out.reason = out.sample.reason;
        if (out.reason == BudgetReason::None)
        {
            if (!key.deviceGeneration || key.deviceGeneration != source.Generation() || textures.empty())
                out.reason = BudgetReason::Attribution;
            for (const auto& texture : textures)
            {
                const auto& memory = out.sample.memory;
                if (!texture.bytes || texture.memoryType >= memory.memoryTypeCount ||
                    memory.memoryTypes[texture.memoryType].heapIndex != out.sample.heap ||
                    !(memory.memoryTypes[texture.memoryType].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                {
                    out.reason = BudgetReason::Attribution;
                    break;
                }
                if (texture.bytes > UINT64_MAX - out.textures)
                {
                    out.reason = BudgetReason::Overflow;
                    break;
                }
                out.textures += texture.bytes;
            }
            if (out.reason == BudgetReason::None && !source.Fresh(out.sample))
                out.reason = BudgetReason::Stale;
            if (out.reason == BudgetReason::None && !Admit(out.sample.budget, out.sample.used, model, out.textures))
                out.reason = BudgetReason::Headroom;
        }
        if (out.reason != BudgetReason::None)
            out.log = Fail(key, out.reason);
        return out;
    }
};
// No unbounded retirement queue: at most two generations including abandoned/unproved ones.
// A lost generation consumes its slot permanently in this owner. CPU tokens own no GPU object.
class RetainedGenerations
{
    struct Slot
    {
        uint64_t id = 0;
        bool abandoned = false;
    };
    std::array<Slot, 2> _slots {};
    uint64_t _next = 1;

  public:
    RetainedGenerations() = default;
    RetainedGenerations(const RetainedGenerations&) = delete;
    RetainedGenerations& operator=(const RetainedGenerations&) = delete;
    uint64_t Reserve(uint32_t count)
    {
        if (count <= 1 || !_next)
            return 0;
        for (auto& slot : _slots)
            if (!slot.id)
            {
                slot.id = _next;
                _next = _next == UINT64_MAX ? 0 : _next + 1;
                return slot.id;
            }
        return 0;
    }
    void Abandon(uint64_t token)
    {
        for (auto& slot : _slots)
            if (slot.id == token && token)
                slot.abandoned = true;
    }
    bool Release(uint64_t token, bool recordingsRetired, bool liveDrain)
    {
        if (!token || !recordingsRetired || !liveDrain)
            return false;
        for (auto& slot : _slots)
            if (slot.id == token && !slot.abandoned)
            {
                slot = {};
                return true;
            }
        return false;
    }
};
} // namespace DlssNr::VkChain
