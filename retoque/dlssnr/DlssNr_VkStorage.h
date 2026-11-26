#pragma once

#include <vulkan/vulkan.h>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace DlssNr::VkStorage
{
inline bool Enabled(const VkDeviceCreateInfo* info)
{
    if (!info)
        return false;
    if (info->pEnabledFeatures && info->pEnabledFeatures->shaderStorageImageWriteWithoutFormat)
        return true;
    for (auto node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 &&
            reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->features.shaderStorageImageWriteWithoutFormat)
            return true;
    return false;
}
inline bool Flags2(const VkDeviceCreateInfo* info)
{
    if (info && info->ppEnabledExtensionNames)
        for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
            if (std::strcmp(info->ppEnabledExtensionNames[i], VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME) == 0)
                return true;
    return false;
}
struct Facts
{
    bool feature = false;
    uint32_t apiVersion = 0; // VkPhysicalDeviceProperties::apiVersion, not the instance request
    bool flags2 = false;
    bool PerFormat() const { return apiVersion >= VK_API_VERSION_1_3 || flags2; }
    bool Capable() const { return feature || PerFormat(); }
};
inline Facts Describe(const VkDeviceCreateInfo* info, uint32_t apiVersion)
{
    return { Enabled(info), apiVersion, Flags2(info) };
}
class Devices
{
    struct Device
    {
        Facts facts;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        PFN_vkGetPhysicalDeviceFormatProperties2 query = nullptr;
        std::unordered_map<VkFormat, const char*> formats;
    };
    std::atomic<bool> _empty { true };
    std::mutex _mutex;
    std::unordered_map<VkDevice, std::shared_ptr<Device>> _devices;

  public:
    void Created(VkDevice device, Facts facts, VkPhysicalDevice physical,
                 PFN_vkGetPhysicalDeviceFormatProperties2 query, bool replace = true)
    {
        std::lock_guard lock(_mutex);
        if (!replace && _devices.contains(device) && _devices.at(device)->physical != VK_NULL_HANDLE)
            return;
        _devices[device] = std::make_shared<Device>(Device { facts, physical, query, {} });
        _empty.store(false);
    }
    bool Destroyed(VkDevice device)
    {
        // NR off/internal creates register nothing: teardown then has no lock, allocation or map write.
        if (_empty.load())
            return false;
        std::lock_guard lock(_mutex);
        const auto entry = _devices.find(device);
        if (entry == _devices.end())
            return false;
        _devices.erase(entry);
        _empty.store(_devices.empty());
        return true;
    }
    // Cache immutable format properties per device generation. No driver or log call under the leaf mutex.
    template <class Log> bool Allows(VkDevice device, VkFormat format, Log log)
    {
        std::shared_ptr<Device> record;
        {
            std::lock_guard lock(_mutex);
            auto& entry = _devices[device];
            if (!entry)
            {
                entry = std::make_shared<Device>();
                _empty.store(false);
            }
            record = entry;
            const auto it = record->formats.find(format);
            if (it != record->formats.end())
                return it->second == nullptr;
        }
        const char* reason = nullptr;
        if (!record->facts.Capable())
            reason = "no enabled formatless-write feature, device Vulkan 1.3 or format_feature_flags2";
        else if (!record->facts.feature)
        {
            if (!record->query)
                reason = "format properties2 query unavailable";
            else
            {
                VkFormatProperties3 flags {};
                flags.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
                VkFormatProperties2 properties {};
                properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
                properties.pNext = &flags;
                record->query(record->physical, format, &properties);
                if (!(flags.optimalTilingFeatures & VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT))
                    reason = "format lacks STORAGE_WRITE_WITHOUT_FORMAT";
            }
        }
        bool first = false;
        {
            std::lock_guard lock(_mutex);
            first = record->formats.emplace(format, reason).second;
        }
        if (reason && first)
            log(device, format, reason);
        return reason == nullptr;
    }
};
inline Devices& Registry()
{
    static Devices devices;
    return devices;
}
} // namespace DlssNr::VkStorage
