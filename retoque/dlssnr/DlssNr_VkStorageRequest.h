#pragma once

#include "DlssNr_VkStorage.h"
#include <hooks/Vulkan_OwnedDeviceRequest.h>

namespace DlssNr::VkStorage
{
// Owner decision, 2026-11-26: opt-in NR may request this supported core feature on game devices.
// The application request is immutable, including any Features2 buried behind other pNext nodes.
class Request
{
    VkDeviceCreateInfo _info {};
    VkPhysicalDeviceFeatures _features {};
    std::unique_ptr<VulkanOwnedDeviceRequest> _chain;
    bool _added = false;
    const char* _reason = nullptr;

  public:
    Request(const VkDeviceCreateInfo* info, bool nr, bool internal, bool supported, uint32_t apiVersion)
    {
        if (!info || !nr || internal || Describe(info, apiVersion).Capable())
            return;
        if (!supported)
        {
            _reason = "shaderStorageImageWriteWithoutFormat is unsupported";
            return;
        }
        _info = *info;
        const VkBaseInStructure* features2 = nullptr;
        for (auto node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext)
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
                features2 = node;
        if (features2)
        {
            _chain = std::make_unique<VulkanOwnedDeviceRequest>(_info);
            if (_chain->LastUncopied() != VK_STRUCTURE_TYPE_MAX_ENUM)
            {
                _reason = "unknown device-create chain node prevents a safe Features2 copy";
                return;
            }
            for (auto node = static_cast<const VkBaseInStructure*>(_info.pNext); node; node = node->pNext)
                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
                    const_cast<VkPhysicalDeviceFeatures2*>(reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node))
                        ->features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
            _info.pEnabledFeatures = nullptr;
        }
        else
        {
            if (info->pEnabledFeatures)
                _features = *info->pEnabledFeatures;
            _features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
            _info.pEnabledFeatures = &_features;
        }
        _added = true;
    }
    bool Added() const { return _added; }
    const char* Reason() const { return _reason; }
    const VkDeviceCreateInfo* Info(const VkDeviceCreateInfo* original) const { return _added ? &_info : original; }
};
} // namespace DlssNr::VkStorage
