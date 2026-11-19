#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <mutex>
#include <utility>

namespace DlssNr::VkChain
{
struct QueueTopology
{
    uint32_t eligible = 0, family = UINT32_MAX;
    bool valid = false;
};
// flagsOf returns {queue flags, advertised queue count}; no lookup/allocation in the helper.
template <class FlagsOf> QueueTopology DescribeQueues(const VkDeviceCreateInfo* info, FlagsOf flagsOf)
{
    QueueTopology out;
    if (!info || !info->queueCreateInfoCount || !info->pQueueCreateInfos)
        return out;
    bool valid = true, compute = false;
    for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i)
    {
        const auto& q = info->pQueueCreateInfos[i];
        const auto [flags, available] = flagsOf(q.queueFamilyIndex);
        if (!available || !q.queueCount || q.queueCount > available || q.flags)
            valid = false;
        if (!(flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
            continue;
        if (q.queueCount > UINT32_MAX - out.eligible)
            return {};
        out.eligible += q.queueCount;
        out.family = q.queueFamilyIndex;
        compute = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
    }
    out.valid = valid && out.eligible == 1 && compute;
    return out;
}
enum class QueueRefusal : uint32_t
{
    None,
    Shape,
    UnobservedQueue,
    UnobservedPool,
    PoolFamily,
    Capacity,
};
inline const char* QueueText(QueueRefusal reason)
{
    switch (reason)
    {
    case QueueRefusal::None:
        return "single observed evaluation queue";
    case QueueRefusal::Shape:
        return "exactly one graphics/compute queue with compute support required";
    case QueueRefusal::UnobservedQueue:
        return "evaluation queue was not observed";
    case QueueRefusal::UnobservedPool:
        return "command-pool family was not observed";
    case QueueRefusal::PoolFamily:
        return "command-pool family differs from evaluation queue";
    case QueueRefusal::Capacity:
        return "command-pool observation capacity exhausted";
    }
    return "unknown queue refusal";
}
// One device. Fixed storage; reads and forgetting use atomics only. Remember is active-chain
// work and uses its own leaf mutex. Vulkan externally synchronizes each pool's lifetime.
template <class Mutex = std::mutex> class QueueProof
{
    struct Pool
    {
        std::atomic<VkCommandPool> handle { VK_NULL_HANDLE };
        std::atomic<uint32_t> family { UINT32_MAX };
    };
    QueueTopology _topology;
    std::atomic<VkQueue> _queue { VK_NULL_HANDLE };
    std::atomic<QueueRefusal> _refusal { QueueRefusal::Shape };
    std::atomic<bool> _reported { false };
    std::array<Pool, 128> _pools {};
    Mutex _write;

  public:
    void Configure(QueueTopology topology)
    {
        _topology = topology;
        _queue.store(VK_NULL_HANDLE);
        _refusal.store(topology.valid ? QueueRefusal::None : QueueRefusal::Shape);
        _reported.store(false);
        for (auto& pool : _pools)
            pool.handle.store(VK_NULL_HANDLE);
    }
    void Refuse(QueueRefusal why)
    {
        auto expected = QueueRefusal::None;
        _refusal.compare_exchange_strong(expected, why);
    }
    void Queue(VkQueue queue, uint32_t family, uint32_t index)
    {
        if (!_topology.valid || family != _topology.family || index != 0 || !queue)
            return;
        VkQueue expected = VK_NULL_HANDLE;
        if (!_queue.compare_exchange_strong(expected, queue) && expected != queue)
            Refuse(QueueRefusal::UnobservedQueue);
    }
    void Forget(VkCommandPool handle)
    {
        for (auto& pool : _pools)
            if (pool.handle.load(std::memory_order_acquire) == handle)
                pool.handle.store(VK_NULL_HANDLE, std::memory_order_release);
    }
    void Remember(VkCommandPool handle, uint32_t family)
    {
        if (!handle || _refusal.load() != QueueRefusal::None)
            return;
        std::lock_guard lock(_write);
        for (auto& pool : _pools)
            if (pool.handle.load() == handle)
                return; // nested export/ICD callback for the same successful creation
        for (auto& pool : _pools)
            if (!pool.handle.load())
            {
                pool.family.store(family, std::memory_order_relaxed);
                pool.handle.store(handle, std::memory_order_release);
                return;
            }
        Refuse(QueueRefusal::Capacity);
    }
    bool Accept(VkCommandPool handle)
    {
        if (_refusal.load() != QueueRefusal::None)
            return false;
        if (!_queue.load(std::memory_order_acquire))
            Refuse(QueueRefusal::UnobservedQueue);
        else
        {
            for (const auto& pool : _pools)
                if (handle && pool.handle.load(std::memory_order_acquire) == handle)
                {
                    if (pool.family.load(std::memory_order_relaxed) == _topology.family)
                        return _refusal.load() == QueueRefusal::None;
                    Refuse(QueueRefusal::PoolFamily);
                    return false;
                }
            Refuse(QueueRefusal::UnobservedPool);
        }
        return false;
    }
    QueueRefusal Reason() const { return _refusal.load(); }
    bool Report() { return Reason() != QueueRefusal::None && !_reported.exchange(true); }
};
} // namespace DlssNr::VkChain
