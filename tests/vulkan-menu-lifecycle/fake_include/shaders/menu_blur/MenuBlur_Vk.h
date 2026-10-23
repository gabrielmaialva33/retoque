#pragma once
#include <string>
#include <imgui/imgui.h>
#include <vulkan/vulkan.h>
// Host fake: the blur is only built once the menu is visible, which no case here makes it. A
// construction is recorded as a fault so a case cannot silently start exercising it.
class MenuBlur_Vk
{
  public:
    MenuBlur_Vk(std::string, VkDevice, VkPhysicalDevice);
    ~MenuBlur_Vk() {}
    bool Prepare(VkFormat, VkExtent2D, VkImageUsageFlags) { return false; }
    bool Dispatch(VkCommandBuffer, VkImage) { return false; }
    ImTextureID TextureId() const { return ImTextureID_Invalid; }
    ImVec2 UVScale() const { return ImVec2(1.0f, 1.0f); }
};
