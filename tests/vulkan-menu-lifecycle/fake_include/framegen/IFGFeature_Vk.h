#pragma once
#include <vulkan/vulkan.h>
struct IFGFeature_Vk
{
    static VkFormat WithoutSrgb(VkFormat);
};
