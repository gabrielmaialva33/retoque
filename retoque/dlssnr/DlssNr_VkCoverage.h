#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace DlssNr::VkChain
{
// Installation evidence only. A complete certificate does not prove creation or GPU completion.
struct HookCoverage
{
    enum : uint32_t
    {
        Destroy = 1u << 0,
        Queue = 1u << 1,
        Queue2 = 1u << 2,
        Submit = 1u << 3,
        Submit2 = 1u << 4,
        Submit2Khr = 1u << 5,
        Allocate = 1u << 6,
        Begin = 1u << 7,
        End = 1u << 8,
        Reset = 1u << 9,
        Free = 1u << 10,
        ResetPool = 1u << 11,
        DestroyPool = 1u << 12,
        Required = (1u << 13) - 1,
    };
    uint32_t installed = 0;
    bool Complete() const { return installed == Required; }
    const char* Reason() const
    {
        return Complete() ? "complete" : "required entrypoint missing, detour failed or target capacity exhausted";
    }
};

// Bounded process-wide certificates, with no heap, constructor allocation or reader lock. Publication
// is serialized by the host's EXISTING installation mutex. Device teardown clears its certificate.
// Vulkan forbids using a device concurrently with its destruction; no old-device reader may survive
// handle reuse. Failure/saturation never grants admission, and repeating installation cannot upgrade
// a failed certificate for a live device (some work may already have escaped observation).
class CoverageRegistry
{
    struct Slot
    {
        std::atomic<uint64_t> device { 0 };
        std::atomic<uint32_t> installed { 0 };
    };
    std::array<Slot, 16> _slots {};

  public:
    // Returns true only for a new certificate; callers log exactly once here, including refusals.
    bool Publish(uint64_t device, HookCoverage coverage)
    {
        if (!device)
            return false;
        for (const auto& slot : _slots)
            if (slot.device.load(std::memory_order_acquire) == device)
                return false;
        for (auto& slot : _slots)
            if (slot.device.load(std::memory_order_acquire) == 0)
            {
                slot.installed.store(coverage.installed, std::memory_order_relaxed);
                slot.device.store(device, std::memory_order_release);
                return true;
            }
        return false;
    }
    bool Ready(uint64_t device) const
    {
        if (!device)
            return false;
        for (const auto& slot : _slots)
            if (slot.device.load(std::memory_order_acquire) == device)
                return slot.installed.load(std::memory_order_relaxed) == HookCoverage::Required;
        return false;
    }
    bool Contains(uint64_t device) const
    {
        for (const auto& slot : _slots)
            if (device && slot.device.load(std::memory_order_acquire) == device)
                return true;
        return false;
    }
    void Forget(uint64_t device)
    {
        for (auto& slot : _slots)
        {
            auto expected = device;
            if (device)
                slot.device.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
        }
    }
};
} // namespace DlssNr::VkChain
