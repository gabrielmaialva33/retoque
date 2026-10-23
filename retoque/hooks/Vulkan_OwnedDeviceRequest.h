#pragma once

// Storage one vkCreateDevice call owns for the request it hands the driver (Retoque; used by
// OptiScaler/hooks/Vulkan_Hooks.cpp's hkvkCreateDevice).
//
// The hook enables features and adds extensions on the way down. Without this it did that in the game's
// own memory: VulkanDeviceFeatureState copies only the structures it knows from the head of the chain,
// so a VkPhysicalDeviceVulkan12Features behind a VkPhysicalDeviceVulkan11Features, or behind any
// structure it does not know, was written in place and restored after the call. A game that keeps its
// chain in read-only memory faults there, and another thread reading that chain meanwhile sees values
// the game never set. Here the whole chain is copied first, so every write lands in the call's copy.
//
// The extension array is copied too, once the spoofing has built it: its list is per thread, and a
// create made on the same thread from inside the driver's call would otherwise rebuild the very array
// the outer request still points at.

#include <hooks/Vulkan_DeviceCreateChainTypes.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

class VulkanOwnedDeviceRequest
{
  public:
    // Replaces createInfo.pNext with a copy owned by this object. A structure whose size is not known (one
    // newer than the build's Vulkan headers) cannot be copied: the chain is the game's from there on, as
    // before, and LastUncopied() names it.
    explicit VulkanOwnedDeviceRequest(VkDeviceCreateInfo& createInfo)
    {
        VkBaseOutStructure* last = nullptr;
        const auto link = [&](const void* node)
        {
            if (last == nullptr)
                createInfo.pNext = node;
            else
                last->pNext = static_cast<VkBaseOutStructure*>(const_cast<void*>(node));
        };

        for (auto* node = static_cast<const VkBaseInStructure*>(createInfo.pNext); node != nullptr; node = node->pNext)
        {
            const size_t size = SizeOf(node->sType);
            if (size == 0)
            {
                _uncopied = node->sType;
                link(node);
                return;
            }

            auto& block = _structs.emplace_back(
                std::make_unique<std::max_align_t[]>((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t)));
            auto* copy = reinterpret_cast<VkBaseOutStructure*>(block.get());
            std::memcpy(copy, node, size);
            copy->pNext = nullptr;
            link(copy);
            last = copy;
        }

        link(nullptr);
    }

    // Points createInfo at a copy of its extension array owned by this object. The names themselves are
    // the game's or string literals, and outlive the call either way.
    void OwnExtensions(VkDeviceCreateInfo& createInfo)
    {
        if (createInfo.enabledExtensionCount == 0)
            return;

        _extensions.assign(createInfo.ppEnabledExtensionNames,
                           createInfo.ppEnabledExtensionNames + createInfo.enabledExtensionCount);
        createInfo.ppEnabledExtensionNames = _extensions.data();
    }

    // The sType the copy stopped at, or VK_STRUCTURE_TYPE_MAX_ENUM if the whole chain is owned
    VkStructureType LastUncopied() const { return _uncopied; }

    VulkanOwnedDeviceRequest(const VulkanOwnedDeviceRequest&) = delete;
    VulkanOwnedDeviceRequest& operator=(const VulkanOwnedDeviceRequest&) = delete;

  private:
    static size_t SizeOf(VkStructureType sType)
    {
        for (const auto& type : VulkanDeviceCreateChain::kTypes)
        {
            if (type.sType == sType)
                return type.size;
        }

        return 0;
    }

    std::vector<std::unique_ptr<std::max_align_t[]>> _structs;
    std::vector<const char*> _extensions;
    VkStructureType _uncopied = VK_STRUCTURE_TYPE_MAX_ENUM;
};
