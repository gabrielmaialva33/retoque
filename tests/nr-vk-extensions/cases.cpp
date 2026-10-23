// The device extension list DLSS-NR's vkCreateDevice hook hands on, against the one rule the spec puts on
// it that the merge can break: VK_KHR_buffer_device_address and VK_EXT_buffer_device_address must not be
// enabled together (VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-03328). OptiScaler's spoofing adds the
// EXT one on NVIDIA, the NR merge adds the KHR one the model names; the production header is compiled as it
// is, against the real Vulkan headers.
#include <dlssnr/DlssNr_VkExtensions.h>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace
{

const char* const kKhr = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
const char* const kExt = VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
const char* const kSwapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
const char* const kBinaryImport = "VK_NVX_binary_import";

bool Has(const std::vector<const char*>& names, const char* name)
{
    return DlssNr::VkExt::ListHas(names.data(), (uint32_t) names.size(), name);
}

bool Drop(std::vector<const char*>& names, const std::vector<const char*>& game)
{
    return DlssNr::VkExt::DropConflictingBufferDeviceAddress(names, game.data(), (uint32_t) game.size());
}

} // namespace

int main()
{
    unsigned cases = 0;
    const auto CASE = [&cases](const char* name)
    {
        ++cases;
        std::cout << "  " << name << "\n";
    };

    {
        const std::vector<const char*> game { kSwapchain };
        std::vector<const char*> names { kSwapchain, kExt, kBinaryImport, kKhr };
        assert(Drop(names, game));
        assert(Has(names, kKhr) && !Has(names, kExt) && Has(names, kSwapchain) && Has(names, kBinaryImport));
        assert(names.size() == 3);
        CASE("the game asked for neither: the EXT the spoofing added goes, the model's KHR stays");
    }
    {
        const std::vector<const char*> game { kSwapchain, kKhr };
        std::vector<const char*> names { kSwapchain, kKhr, kExt };
        assert(Drop(names, game));
        assert(Has(names, kKhr) && !Has(names, kExt));
        CASE("the game asked for the KHR one: the EXT the spoofing added goes");
    }
    {
        const std::vector<const char*> game { kSwapchain, kExt };
        std::vector<const char*> names { kSwapchain, kExt, kKhr };
        assert(Drop(names, game));
        assert(Has(names, kExt) && !Has(names, kKhr));
        CASE("the game asked for the EXT one: it is kept and the KHR one is not added");
    }
    {
        const std::vector<const char*> game { kKhr, kExt };
        std::vector<const char*> names { kKhr, kExt };
        assert(!Drop(names, game));
        assert(names.size() == 2);
        CASE("the game asked for both: its own list, left as it is");
    }
    {
        const std::vector<const char*> game {};
        std::vector<const char*> onlyKhr { kSwapchain, kKhr };
        std::vector<const char*> onlyExt { kSwapchain, kExt };
        std::vector<const char*> neither { kSwapchain };
        assert(!Drop(onlyKhr, game) && onlyKhr.size() == 2);
        assert(!Drop(onlyExt, game) && onlyExt.size() == 2);
        assert(!Drop(neither, game) && neither.size() == 1);
        CASE("one of them, or neither: nothing changes");
    }
    {
        VkPhysicalDeviceVulkan14Features vulkan14 {};
        vulkan14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
        VkPhysicalDeviceVulkan12Features vulkan12 {};
        vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan12.pNext = &vulkan14;

        assert(!DlssNr::VkExt::PushDescriptorFeatureOff(nullptr));
        assert(DlssNr::VkExt::PushDescriptorFeatureOff(&vulkan12));
        vulkan14.pushDescriptor = VK_TRUE;
        assert(!DlssNr::VkExt::PushDescriptorFeatureOff(&vulkan12));
        vulkan12.pNext = nullptr;
        assert(!DlssNr::VkExt::PushDescriptorFeatureOff(&vulkan12));
        CASE("push_descriptor: refused only behind a Vulkan 1.4 feature struct that has it off");
    }

    std::cout << "vk extensions: " << cases << " cases passed\n";
    return 0;
}
