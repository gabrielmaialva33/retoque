#include <cassert>
#include <cstring>
template <class... Args> void Log(Args&&...) {}
#define LOG_WARN(...) Log(__VA_ARGS__)
#define LOG_INFO(...) Log(__VA_ARGS__)
#include <dlssnr/DlssNr_VkStorageRuntime.h>

// Compile and execute the actual function-resolution adapter without a Vulkan driver.
static void Properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties* properties)
{
    assert(physical == reinterpret_cast<VkPhysicalDevice>(2));
    properties->apiVersion = VK_API_VERSION_1_3;
}
static void Features(VkPhysicalDevice, VkPhysicalDeviceFeatures* features)
{
    features->shaderStorageImageWriteWithoutFormat = VK_TRUE;
}
static void Formats(VkPhysicalDevice, VkFormat, VkFormatProperties2* properties)
{
    static_cast<VkFormatProperties3*>(properties->pNext)->optimalTilingFeatures =
        VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
}
static PFN_vkVoidFunction Get(VkInstance instance, const char* name)
{
    assert(instance == reinterpret_cast<VkInstance>(1));
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(Properties);
    if (std::strcmp(name, "vkGetPhysicalDeviceFeatures") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(Features);
    if (std::strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(Formats);
    return nullptr;
}
int main()
{
    using namespace DlssNr::VkStorage;
    const auto pd = reinterpret_cast<VkPhysicalDevice>(2);
    const auto physical = Query(pd, reinterpret_cast<VkInstance>(1), Get, VK_API_VERSION_1_0);
    assert(physical.apiVersion == VK_API_VERSION_1_3 && physical.supported && physical.formats == Formats);
    assert(!Query(pd, VK_NULL_HANDLE, Get, 0).supported);
    const auto d = reinterpret_cast<VkDevice>(3);
    Registry().Created(d, { false, physical.apiVersion, false }, pd, physical.formats);
    // The outer hook must not overwrite the lower host's final effective request.
    Registry().Created(d, {}, pd, nullptr, false);
    assert(Allows(d, { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT }));
    Registry().Destroyed(d);
    assert(!Allows(d, { VK_FORMAT_R16G16B16A16_SFLOAT }));
    // An early unknown observation must not prevent later successful create observation.
    Registry().Created(d, { true, VK_API_VERSION_1_0, false }, pd, nullptr, false);
    assert(Allows(d, { VK_FORMAT_R16G16B16A16_SFLOAT }));
}
