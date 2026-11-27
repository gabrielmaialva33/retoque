#pragma once

#include "DlssNr_VkStorageRequest.h"
#include <initializer_list>

namespace DlssNr::VkStorage
{
inline bool Allows(VkDevice device, std::initializer_list<VkFormat> formats)
{
    for (auto format : formats)
        if (!Registry().Allows(device, format,
                               [](VkDevice d, VkFormat f, const char* reason)
                               {
                                   LOG_WARN("DLSS-NR Vulkan: storage format {} refused on device {}: {}", (int) f,
                                            (void*) d, reason);
                               }))
            return false;
    return true;
}
struct PhysicalFacts
{
    uint32_t apiVersion = 0;
    bool supported = false;
    PFN_vkGetPhysicalDeviceFormatProperties2 formats = nullptr;
};
inline PhysicalFacts Query(VkPhysicalDevice physical, VkInstance instance, PFN_vkGetInstanceProcAddr get,
                           uint32_t instanceApiVersion)
{
    PhysicalFacts result;
    if (!get || !instance)
        return result;
    const auto properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(instance, "vkGetPhysicalDeviceProperties"));
    const auto features =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(get(instance, "vkGetPhysicalDeviceFeatures"));
    if (properties)
    {
        VkPhysicalDeviceProperties value {};
        properties(physical, &value);
        result.apiVersion = value.apiVersion;
    }
    if (features)
    {
        VkPhysicalDeviceFeatures value {};
        features(physical, &value);
        result.supported = value.shaderStorageImageWriteWithoutFormat != 0;
    }
    if (instanceApiVersion >= VK_API_VERSION_1_1)
        result.formats = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
            get(instance, "vkGetPhysicalDeviceFormatProperties2"));
    if (!result.formats)
        result.formats = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
            get(instance, "vkGetPhysicalDeviceFormatProperties2KHR"));
    return result;
}
inline void Note(const Request& request)
{
    if (request.Added())
        LOG_INFO("DLSS-NR Vulkan: enabled supported shaderStorageImageWriteWithoutFormat for this game device");
    else if (request.Reason())
        LOG_WARN("DLSS-NR Vulkan: formatless-write request refused: {}", request.Reason());
}
} // namespace DlssNr::VkStorage
