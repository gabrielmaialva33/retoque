#pragma once

#include "SysUtils.h"
#include <vulkan/vulkan.h>

namespace MenuOverlayVk
{
void CreateSwapchain(VkDevice device, VkPhysicalDevice pd, VkInstance instance, HWND hwnd,
                     const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                     VkSwapchainKHR* pSwapchain);
// Records which queue families a device was created with. vkGetDeviceQueue on a family or index the
// device never created is undefined behaviour, and vkCreateDevice is the only place that knows.
void DeviceCreated(VkDevice device, const VkDeviceCreateInfo* pCreateInfo);
bool QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo);
// With a window, asks OnOtherWindow(hwnd) again under the overlay's locks and keeps the menu (returns
// false) when it is on another window by then: the caller's own check ran before them
bool DestroyVulkanObjects(bool shutdown, HWND hwnd = nullptr);

// The menu stays on its window while that exists, games can have more than one
bool OnOtherWindow(HWND hwnd);
} // namespace MenuOverlayVk
