#pragma once
// Deliberately narrow, typed host boundary for the Vulkan functions exercised by
// the production meter allocator. These names, members and call signatures match
// Vulkan; this fixture is not a Vulkan ABI, loader or device test.
#include <cstdint>
using VkDevice = struct VkDevice_T*;
using VkBuffer = uint64_t;
using VkDeviceMemory = uint64_t;
using VkDeviceSize = uint64_t;
using VkFlags = uint32_t;
using VkMemoryPropertyFlags = VkFlags;
using VkMemoryMapFlags = VkFlags;
using VkBufferCreateFlags = VkFlags;
using VkBufferUsageFlags = VkFlags;
struct VkAllocationCallbacks;
constexpr uint64_t VK_NULL_HANDLE = 0;
enum VkResult
{
    VK_SUCCESS = 0,
    VK_ERROR_OUT_OF_HOST_MEMORY = -1,
    VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
    VK_ERROR_MEMORY_MAP_FAILED = -5
};
enum VkStructureType
{
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12
};
enum VkSharingMode
{
    VK_SHARING_MODE_EXCLUSIVE = 0
};
constexpr VkBufferUsageFlags VK_BUFFER_USAGE_TRANSFER_DST_BIT = 0x2;
constexpr VkMemoryPropertyFlags VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x2;
constexpr VkMemoryPropertyFlags VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x4;
struct VkBufferCreateInfo
{
    VkStructureType sType;
    const void* pNext;
    VkBufferCreateFlags flags;
    VkDeviceSize size;
    VkBufferUsageFlags usage;
    VkSharingMode sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
};
struct VkMemoryRequirements
{
    VkDeviceSize size, alignment;
    uint32_t memoryTypeBits;
};
struct VkMemoryAllocateInfo
{
    VkStructureType sType;
    const void* pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeIndex;
};
