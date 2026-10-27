#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
void TestLayout(VkCommandBuffer, VkImage, VkImageLayout, VkImageLayout);
class Shader_Vk
{
  protected:
    std::string _name;
    bool _init = false;
    VkDevice _device = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> _descriptorSets;
    VkBuffer _constantBuffer = VK_NULL_HANDLE;
    void* _mappedConstantBuffer = nullptr;
    VkSampler _textureSampler = VK_NULL_HANDLE;
    static uint32_t FindMemoryType(VkPhysicalDevice, uint32_t, VkMemoryPropertyFlags) { return 0; }
    void SetImageLayout(VkCommandBuffer command, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                        VkImageSubresourceRange)
    {
        TestLayout(command, image, oldLayout, newLayout);
    }

  public:
    Shader_Vk(std::string name, VkDevice device, VkPhysicalDevice physical)
        : _name(std::move(name)), _device(device), _physicalDevice(physical)
    {
    }
    virtual ~Shader_Vk() = default;
    bool CanRender() const { return _init && _pipeline != VK_NULL_HANDLE; }
};
