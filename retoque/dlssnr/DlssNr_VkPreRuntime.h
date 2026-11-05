#pragma once

#include "DlssNr_VkPreObserve.h"
#include <vulkan/vulkan.h>

namespace DlssNr::VkPre::Runtime
{
// Passive, startup opt-in at Enabled && Stage==1. This observer itself never substitutes resources or records work.
void Install(VkDevice device, const VkDeviceCreateInfo* info = nullptr, uint32_t apiVersion = 0) noexcept;
void RememberFeature(VkDevice device, uint64_t handle, Feature kind, bool success, uint64_t source = 0) noexcept;
void ForgetFeature(uint64_t source, uint64_t handle) noexcept;
// Snapshot consumers must still prove hook coverage, ownership, execution and layout compatibility.
// Null before the first opt-in device; querying does not allocate.
Observations* Facts() noexcept;
bool Uninterrupted() noexcept;
// Only the ticket captured across the same Begin and still current; never repairs missing observations.
std::optional<Ticket> CurrentTicket(VkDevice device, VkCommandBuffer buffer) noexcept;
struct HookStatus
{
    uint32_t attached = 0, missing = 0, failed = 0;
    uint32_t requiredMissing = 0, requiredFailed = 0;
    bool complete = false, formatless = false, formatFlags2 = false;
};
// Completeness of required lifecycle/layout/rendering targets; binding diagnostics are optional.
// Never a GPU/execution/ownership qualification.
HookStatus Status(VkDevice device) noexcept;
} // namespace DlssNr::VkPre::Runtime
