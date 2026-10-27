#pragma once
// The CPU/GPU boundary is fake; production scaler constructor and dispatch bodies are unmodified.
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <memory>
#include <optional>
struct VkImageInfo
{
    VkImageView ImageView;
    VkImage Image;
    VkImageSubresourceRange SubresourceRange;
    VkFormat Format;
    unsigned int Width;
    unsigned int Height;
};
struct GpuTime_Vk
{
};
class Shader_Vk
{
  protected:
    std::string _name;
    bool _init = false;
    VkDevice _device = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    std::unique_ptr<GpuTime_Vk> GpuTime;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout _descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> _descriptorSets;
    uint32_t _currentSetIndex = 0, _maxFramesInFlight = 3;
    VkBuffer _constantBuffer = VK_NULL_HANDLE;
    VkDeviceMemory _constantBufferMemory = VK_NULL_HANDLE;
    void* _mappedConstantBuffer = nullptr;
    VkSampler _textureSampler = VK_NULL_HANDLE;
    static uint32_t FindMemoryType(VkPhysicalDevice, uint32_t, VkMemoryPropertyFlags);
    static bool CreateBufferResource(VkDevice, VkPhysicalDevice, VkBuffer*, VkDeviceMemory*, VkDeviceSize,
                                     VkBufferUsageFlags, VkMemoryPropertyFlags);
    void CreateConstantBuffer(VkDeviceSize);
    bool CreateImageResource(uint32_t, uint32_t, VkFormat, VkImageUsageFlags) { return false; }
    void SetImageLayout(VkCommandBuffer, VkImage, VkImageLayout, VkImageLayout, VkImageSubresourceRange) {}

  public:
    Shader_Vk(std::string name, VkDevice device, VkPhysicalDevice physical)
        : _name(std::move(name)), _device(device), _physicalDevice(physical)
    {
    }
    virtual ~Shader_Vk();
    bool IsInit() const { return _init; }
    bool CanRender() const { return _init && _pipeline != VK_NULL_HANDLE; }
};
