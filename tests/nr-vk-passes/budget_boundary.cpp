#include "pch.h"
#include "DlssNr_VkChain.h"
#include <vulkan/vulkan.h>
#include <cassert>
#include <iostream>

// Counterexample, not a model/GPU trace: two legal device-local memory types select
// different heaps even for otherwise identical images. Production chooses by typeBits.
static VkPhysicalDeviceMemoryProperties properties {};
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out)
{
    *out = properties;
}

// @production@

int main()
{
    constexpr uint64_t GiB = 1ull << 30, MiB = 1ull << 20;
    properties.memoryHeapCount = 2;
    properties.memoryTypeCount = 2;
    for (unsigned i = 0; i != 2; ++i)
    {
        properties.memoryHeaps[i] = { 8 * GiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };
        properties.memoryTypes[i] = { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, i };
    }
    const auto first = FindMemoryTypeIndexOn(VK_NULL_HANDLE, 1, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const auto second = FindMemoryTypeIndexOn(VK_NULL_HANDLE, 2, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    assert(first == 0 && second == 1);
    assert(properties.memoryTypes[first].heapIndex != properties.memoryTypes[second].heapIndex);
    assert(FindMemoryTypeIndexOn(VK_NULL_HANDLE, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == UINT32_MAX);

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget {};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    budget.heapBudget[0] = budget.heapBudget[1] = 8 * GiB;
    budget.heapUsage[0] = 7 * GiB;
    budget.heapUsage[1] = GiB;
    // The same scalar model estimate cannot identify which of these decisions is relevant.
    constexpr uint64_t model = 256 * MiB;
    assert(!DlssNr::VkChain::Admit(budget.heapBudget[0], budget.heapUsage[0], model, 0));
    assert(DlssNr::VkChain::Admit(budget.heapBudget[1], budget.heapUsage[1], model, 0));
    assert(DlssNr::VkChain::Admit(budget.heapBudget[0] + budget.heapBudget[1],
                                  budget.heapUsage[0] + budget.heapUsage[1], model, 0));
    std::cout << "PASS: actual memory-type selection can use different heaps; aggregate admission hides refusal\n";
}
