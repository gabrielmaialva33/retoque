#pragma once
namespace VulkanHooks
{
PFN_vkVoidFunction GetDeviceProcAddr(VkDevice, const char*);
}
