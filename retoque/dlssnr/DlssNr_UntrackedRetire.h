#pragma once

// GPU-completion gate for the default (untracked) D3D12 NR retirement path.
//
// On the untracked path (g_tracked == false), parked NR objects keep their 32-tick evaluation
// countdown and add an independent present-queue fence requirement: once a D3D12 swapchain
// Present has been observed on a single queue for the device, Signal(fence, serial) runs after
// each successful Present on that queue. Park records the serial that will be signaled at the
// next Present; an untracked retired entry may be freed only after its count gate has expired AND
// fence->GetCompletedValue() >= parkedSerial + 1 (at least one full Present after the park
// completed on the GPU).
//
// UINT64_MAX (DXGI_ERROR_DEVICE_REMOVED) and device-removed errors never count as completion and
// never fall back to count-only release. When no present-queue fence is available on a live
// device (no D3D12 swapchain seen, multiple present queues on the device, slot table full, or
// CreateFence/Signal failure on a live device), retirement falls back to the exact count-only
// rule and logs the reason once at INFO.
//
// Standard library only; tested by tests/nr-untracked-retire.

#include <cstdint>

namespace DlssNr::UntrackedRetire
{
enum class UnavailableReason : uint8_t
{
    None = 0,
    NoSwapchainPresent,
    MultiplePresentQueues,
    SlotLimitReached,
    CreateFenceFailed,
    SignalFailed,
};

constexpr const char* DescribeUnavailable(UnavailableReason reason)
{
    switch (reason)
    {
    case UnavailableReason::None:
        return "present-queue fence active";
    case UnavailableReason::NoSwapchainPresent:
        return "no D3D12 swapchain present has been seen on this device";
    case UnavailableReason::MultiplePresentQueues:
        return "more than one present queue was seen on this device";
    case UnavailableReason::SlotLimitReached:
        return "the present-fence device table is full";
    case UnavailableReason::CreateFenceFailed:
        return "CreateFence failed on the present queue's device";
    case UnavailableReason::SignalFailed:
        return "Signal failed on the swapchain's present queue";
    }
    return "present-queue fence unavailable";
}

constexpr int StepCountdown(int framesLeft) { return framesLeft > 0 ? framesLeft - 1 : 0; }

// Reserves UINT64_MAX for D3D12's GetCompletedValue() device-removed sentinel.
constexpr uint64_t NextSignalValue(uint64_t lastSignaled)
{
    return lastSignaled < UINT64_MAX - 1 ? lastSignaled + 1 : UINT64_MAX - 1;
}

struct Query
{
    bool hasFence = false;
    bool deviceRemoved = false;
    UnavailableReason unavailable = UnavailableReason::NoSwapchainPresent;
    uint64_t completedValue = 0;
};

constexpr bool CanFree(int framesLeftAfterStep, uint64_t parkedSerial, const Query& query)
{
    if (framesLeftAfterStep > 0)
        return false;
    if (query.deviceRemoved)
        return false;
    if (!query.hasFence)
        return true;
    if (query.completedValue == UINT64_MAX)
        return false;
    if (parkedSerial >= UINT64_MAX - 1)
        return false;
    return query.completedValue >= parkedSerial + 1;
}
} // namespace DlssNr::UntrackedRetire
