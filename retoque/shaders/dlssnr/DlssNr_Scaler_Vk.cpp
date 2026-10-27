#include "pch.h"
#include "DlssNr_Scaler_Vk.h"
#include <Config.h>
#include <shaders/output_scaling/OS_Common.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#define A_CPU
#include <shaders/output_scaling/fsr1/ffx_fsr1.h>

DlssNr_Scaler_Vk::DlssNr_Scaler_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice,
                                   bool InUpsample, Scaler InScaler)
    : OS_Vk(std::move(InName), InDevice, InPhysicalDevice, InUpsample, InScaler), _scaler(InScaler),
      _upsample(InUpsample)
{
    const bool baseReady = CanRender();
    _init = false;
    if (!baseReady || _device == VK_NULL_HANDLE || _physicalDevice == VK_NULL_HANDLE ||
        static_cast<uint32_t>(_scaler) >= static_cast<uint32_t>(Scaler::Count) || _pipelineLayout == VK_NULL_HANDLE ||
        _textureSampler == VK_NULL_HANDLE || _descriptorSets.size() < kSlots)
        return;
    for (uint32_t slot = 0; slot < kSlots; ++slot)
    {
        if (_descriptorSets[slot] == VK_NULL_HANDLE)
            return;
    }

    VkPhysicalDeviceProperties properties {};
    vkGetPhysicalDeviceProperties(_physicalDevice, &properties);
    _constantSize = _scaler == Scaler::FSR1 ? sizeof(UpscaleShaderConstants) : sizeof(Constants);
    const VkDeviceSize alignment = std::max<VkDeviceSize>(1, properties.limits.minUniformBufferOffsetAlignment);
    if (_constantSize > properties.limits.maxUniformBufferRange ||
        alignment - 1 > std::numeric_limits<VkDeviceSize>::max() - _constantSize)
        return;
    _slotStride = ((_constantSize + alignment - 1) / alignment) * alignment;
    if (_slotStride > std::numeric_limits<VkDeviceSize>::max() / kSlots)
        return;
    _maxGroupsX = properties.limits.maxComputeWorkGroupCount[0];
    _maxGroupsY = properties.limits.maxComputeWorkGroupCount[1];
    if (_maxGroupsX == 0 || _maxGroupsY == 0)
        return;

    // The base constructor has never recorded a dispatch or written these descriptors. Replace
    // only its unused uniform allocation, retaining its compiled pipeline and three descriptor sets.
    if (_mappedConstantBuffer != nullptr)
        vkUnmapMemory(_device, _constantBufferMemory);
    if (_constantBuffer != VK_NULL_HANDLE)
        vkDestroyBuffer(_device, _constantBuffer, nullptr);
    if (_constantBufferMemory != VK_NULL_HANDLE)
        vkFreeMemory(_device, _constantBufferMemory, nullptr);
    _mappedConstantBuffer = nullptr;
    _constantBuffer = VK_NULL_HANDLE;
    _constantBufferMemory = VK_NULL_HANDLE;

    // The shared helper does not check vkBindBufferMemory. Allocate this fork-owned buffer
    // transactionally and publish handles only after allocation, binding and mapping succeed.
    VkBufferCreateInfo bufferInfo {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = _slotStride * kSlots;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer candidateBuffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(_device, &bufferInfo, nullptr, &candidateBuffer) != VK_SUCCESS)
        return;

    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(_device, candidateBuffer, &requirements);
    const uint32_t memoryType =
        FindMemoryType(_physicalDevice, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == std::numeric_limits<uint32_t>::max())
    {
        vkDestroyBuffer(_device, candidateBuffer, nullptr);
        return;
    }
    VkMemoryAllocateInfo allocation {};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    VkDeviceMemory candidateMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(_device, &allocation, nullptr, &candidateMemory) != VK_SUCCESS)
    {
        vkDestroyBuffer(_device, candidateBuffer, nullptr);
        return;
    }
    const auto discardCandidate = [&]()
    {
        vkDestroyBuffer(_device, candidateBuffer, nullptr);
        vkFreeMemory(_device, candidateMemory, nullptr);
    };
    if (vkBindBufferMemory(_device, candidateBuffer, candidateMemory, 0) != VK_SUCCESS)
    {
        discardCandidate();
        return;
    }
    void* candidateMapped = nullptr;
    if (vkMapMemory(_device, candidateMemory, 0, bufferInfo.size, 0, &candidateMapped) != VK_SUCCESS)
    {
        discardCandidate();
        return;
    }
    if (candidateMapped == nullptr)
    {
        vkUnmapMemory(_device, candidateMemory);
        discardCandidate();
        return;
    }
    _constantBuffer = candidateBuffer;
    _constantBufferMemory = candidateMemory;
    _mappedConstantBuffer = candidateMapped;
    _init = true;
}

std::optional<DlssNr_Scaler_Vk::Batch> DlssNr_Scaler_Vk::Reserve(VkCommandBuffer cmd, uint32_t count)
{
    if (!CanRender() || cmd == VK_NULL_HANDLE || _mappedConstantBuffer == nullptr ||
        _constantBuffer == VK_NULL_HANDLE || _constantBufferMemory == VK_NULL_HANDLE)
        return std::nullopt;
    auto& recordings = DlssNr::VkRecording::Recordings();
    const auto ticket = recordings.Current((uint64_t) _device, (uint64_t) cmd);
    if (!ticket)
        return std::nullopt;
    auto lease = _slots.Acquire(count);
    if (!lease)
        return std::nullopt;
    try
    {
        if (!recordings.Pin(*ticket, { lease }))
            return std::nullopt;
    }
    catch (const std::bad_alloc&)
    {
        return std::nullopt;
    }
    return Batch(std::move(lease), cmd, this, *ticket);
}

bool DlssNr_Scaler_Vk::Dispatch(VkCommandBuffer InCmdList, Batch& batch, const VkImageInfo& InResourceView,
                                const VkImageInfo& OutResourceView)
{
    if (!CanRender() || InCmdList == VK_NULL_HANDLE || batch.owner != this || batch.commandBuffer != InCmdList ||
        batch.ticket.device != (uint64_t) _device || batch.ticket.buffer != (uint64_t) InCmdList || !batch.lease ||
        batch.next >= batch.lease->slots.size())
        return false;
    const auto current = DlssNr::VkRecording::Recordings().Current((uint64_t) _device, (uint64_t) InCmdList);
    if (!current || current->epoch != batch.ticket.epoch)
        return false;

    const uint32_t slot = batch.lease->slots[batch.next];
    if (slot >= kSlots || slot >= _descriptorSets.size() || _descriptorSets[slot] == VK_NULL_HANDLE ||
        _mappedConstantBuffer == nullptr || _constantBuffer == VK_NULL_HANDLE ||
        _constantBufferMemory == VK_NULL_HANDLE || _pipelineLayout == VK_NULL_HANDLE ||
        _textureSampler == VK_NULL_HANDLE || _slotStride < _constantSize || InResourceView.Image == VK_NULL_HANDLE ||
        InResourceView.ImageView == VK_NULL_HANDLE || OutResourceView.Image == VK_NULL_HANDLE ||
        OutResourceView.ImageView == VK_NULL_HANDLE)
        return false;

    const uint32_t srcW = InResourceView.Width, srcH = InResourceView.Height;
    const uint32_t dstW = OutResourceView.Width, dstH = OutResourceView.Height;
    const uint32_t maxDimension = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
    if (srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0 || srcW > maxDimension || srcH > maxDimension ||
        dstW > maxDimension || dstH > maxDimension)
        return false;
    const uint32_t groupSize = _scaler == Scaler::FSR1 || _upsample ? 16 : 8;
    const uint32_t groupsX = (dstW + groupSize - 1) / groupSize;
    const uint32_t groupsY = (dstH + groupSize - 1) / groupSize;
    if (groupsX > _maxGroupsX || groupsY > _maxGroupsY)
        return false;

    // Admission precedes every mapped write, descriptor update and GPU command. The outer NR
    // timer covers these passes; per-pass timestamp recording must not bypass this admission.
    const VkDeviceSize offset = _slotStride * slot;
    if (_scaler == Scaler::FSR1)
    {
        UpscaleShaderConstants constants {};
        FsrEasuCon(constants.const0, constants.const1, constants.const2, constants.const3, static_cast<float>(srcW),
                   static_cast<float>(srcH), static_cast<float>(srcW), static_cast<float>(srcH),
                   static_cast<float>(dstW), static_cast<float>(dstH));
        std::memcpy(static_cast<char*>(_mappedConstantBuffer) + offset, &constants, sizeof(constants));
    }
    else
    {
        Constants constants {};
        constants.srcWidth = static_cast<int32_t>(srcW);
        constants.srcHeight = static_cast<int32_t>(srcH);
        constants.destWidth = static_cast<int32_t>(dstW);
        constants.destHeight = static_cast<int32_t>(dstH);
        std::memcpy(static_cast<char*>(_mappedConstantBuffer) + offset, &constants, sizeof(constants));
    }
    ++batch.next;

    const VkDescriptorSet currentSet = _descriptorSets[slot];
    VkDescriptorBufferInfo bufferInfo { _constantBuffer, offset, _constantSize };
    VkDescriptorImageInfo sourceInfo { VK_NULL_HANDLE, InResourceView.ImageView,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo destInfo { VK_NULL_HANDLE, OutResourceView.ImageView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo samplerInfo { _textureSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet writes[] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, currentSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          nullptr, &bufferInfo, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, currentSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          &sourceInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, currentSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          &destInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, currentSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,
          &samplerInfo, nullptr, nullptr },
    };
    vkUpdateDescriptorSets(_device, 4, writes, 0, nullptr);
    vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &currentSet, 0, nullptr);
    vkCmdDispatch(InCmdList, groupsX, groupsY, 1);
    return true;
}
