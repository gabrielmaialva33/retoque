#include "pch.h"

#include "DlssNr_GuideMatch_Vk.h"

// Not optional, unlike the D3D12 pass's header: on this route a resample that is wanted and missing refuses the frame,
// so a build without the bytecode would quietly stop NR below native. A missing header fails the build instead.
#include "precompile/DlssNr_Guides_Shader_Vk.h"

#include <cstring>
#include <iterator>
#include <vector>

namespace
{
using namespace DlssNr::GuideMatch::Vk;

constexpr VkDescriptorSetLayoutBinding kBindings[] = {
    { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // gDepth, the game's
    { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // gMotion, the game's
    { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // gDepthOut, r32f
    { 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // gMotionOut, rg32f
};

VkFormatFeatureFlags OptimalFeatures(VkPhysicalDevice physical, VkFormat format)
{
    VkFormatProperties properties {};
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    return properties.optimalTilingFeatures;
}

bool ImageFits(VkPhysicalDevice physical, VkFormat format, uint32_t width, uint32_t height)
{
    VkImageFormatProperties image {};
    return vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                    kOutputUsage, 0, &image) == VK_SUCCESS &&
           width <= image.maxExtent.width && height <= image.maxExtent.height &&
           (image.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
}
} // namespace

const char* DlssNr_GuideMatch_Vk::Unsupported(VkPhysicalDevice physical, uint32_t width, uint32_t height)
{
    if (physical == VK_NULL_HANDLE)
        return "no physical device";

    return OutputRefusal(OptimalFeatures(physical, kDepthFormat), ImageFits(physical, kDepthFormat, width, height),
                         OptimalFeatures(physical, kMotionFormat), ImageFits(physical, kMotionFormat, width, height));
}

bool DlssNr_GuideMatch_Vk::Create(VkDevice device, VkPhysicalDevice physical, std::string& why)
{
    Destroy();
    _device = device;
    _physical = physical;

    const auto failed = [this, &why](VkResult result, const char* what)
    {
        if (result == VK_SUCCESS)
            return false;

        why = std::string(what) + " could not be made (VkResult " + std::to_string((int) result) + ")";
        // Nothing is called on a lost device: what was made is forgotten with it.
        if (result == VK_ERROR_DEVICE_LOST)
        {
            Abandon();
            _lost = true;
        }
        else
        {
            Destroy();
        }
        return true;
    };

    if (device == VK_NULL_HANDLE || physical == VK_NULL_HANDLE)
    {
        why = "no device";
        Abandon();
        return false;
    }

    VkDescriptorSetLayoutCreateInfo setInfo { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setInfo.bindingCount = (uint32_t) std::size(kBindings);
    setInfo.pBindings = kBindings;
    if (failed(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &_setLayout), "the descriptor set layout"))
        return false;

    // One range, the whole block, compute only, from offset 0: a multiple of 4 and within maxPushConstantsSize.
    VkPushConstantRange range {};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = sizeof(DlssNr::GuideMatch::Vk::Push);

    VkPipelineLayoutCreateInfo layoutInfo { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (failed(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &_layout), "the pipeline layout"))
        return false;

    // The generated array is bytes; pCode must be 4-byte aligned words.
    std::vector<uint32_t> words((sizeof(dlssnr_guides_spv) + 3) / 4, 0u);
    std::memcpy(words.data(), dlssnr_guides_spv, sizeof(dlssnr_guides_spv));

    VkShaderModuleCreateInfo moduleInfo { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    moduleInfo.codeSize = sizeof(dlssnr_guides_spv);
    moduleInfo.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (failed(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "the shader module"))
        return false;

    VkComputePipelineCreateInfo pipelineInfo { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "CSMain";
    pipelineInfo.layout = _layout;
    const VkResult made = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_pipeline);
    if (made != VK_ERROR_DEVICE_LOST)
        vkDestroyShaderModule(device, module, nullptr);
    if (made != VK_SUCCESS)
        _pipeline = VK_NULL_HANDLE;
    if (failed(made, "the compute pipeline"))
        return false;

    const VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kSets },
                                           { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kSets } };
    VkDescriptorPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = kSets;
    poolInfo.poolSizeCount = (uint32_t) std::size(sizes);
    poolInfo.pPoolSizes = sizes;
    if (failed(vkCreateDescriptorPool(device, &poolInfo, nullptr, &_pool), "the descriptor pool"))
        return false;

    VkDescriptorSetLayout layouts[kSets];
    for (VkDescriptorSetLayout& layout : layouts)
        layout = _setLayout;

    VkDescriptorSetAllocateInfo setsInfo { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    setsInfo.descriptorPool = _pool;
    setsInfo.descriptorSetCount = kSets;
    setsInfo.pSetLayouts = layouts;
    if (failed(vkAllocateDescriptorSets(device, &setsInfo, _sets), "the descriptor sets"))
        return false;

    _next = 0;
    return true;
}

const char* DlssNr_GuideMatch_Vk::InputRefusal(VkFormat depthFormat, const VkImageSubresourceRange& depthRange,
                                               VkFormat motionFormat, const VkImageSubresourceRange& motionRange)
{
    if (_physical == VK_NULL_HANDLE)
        return "the resample has no device";

    if (depthFormat != _depthFormat)
    {
        _depthFormat = depthFormat;
        _depthFeatures = OptimalFeatures(_physical, depthFormat);
    }

    if (motionFormat != _motionFormat)
    {
        _motionFormat = motionFormat;
        _motionFeatures = OptimalFeatures(_physical, motionFormat);
    }

    if (const char* why = DepthViewRefusal(depthFormat, depthRange.aspectMask, depthRange.layerCount, _depthFeatures))
        return why;

    return MotionViewRefusal(motionFormat, motionRange.aspectMask, motionRange.layerCount, _motionFeatures);
}

void DlssNr_GuideMatch_Vk::Record(VkCommandBuffer cmd, VkImageView depth, VkImageView motion, VkImageLayout inputLayout,
                                  const DlssNr::GuideMatch::Region& depthRegion,
                                  const DlssNr::GuideMatch::Region& motionRegion, VkImage depthOut,
                                  VkImageView depthOutView, VkImage motionOut, VkImageView motionOutView,
                                  uint32_t outWidth, uint32_t outHeight)
{
    const VkDescriptorSet set = _sets[_next];
    _next = (_next + 1) % kSets;

    // The game's views in the layout NGX takes them in; ours in GENERAL, the one storage images may be in.
    const VkDescriptorImageInfo images[] = {
        { VK_NULL_HANDLE, depth, inputLayout },
        { VK_NULL_HANDLE, motion, inputLayout },
        { VK_NULL_HANDLE, depthOutView, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, motionOutView, VK_IMAGE_LAYOUT_GENERAL },
    };

    VkWriteDescriptorSet writes[4] {};
    for (uint32_t i = 0; i < 4; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = kBindings[i].descriptorType;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(_device, 4, writes, 0, nullptr);

    // Before: whatever used the outputs last -- the model reading them a frame ago, this pass writing them -- is done
    // before this write, and their contents are discarded (UNDEFINED), every texel being written below.
    VkImageMemoryBarrier toWrite[2] {};
    const VkImage outputs[2] = { depthOut, motionOut };
    for (uint32_t i = 0; i < 2; ++i)
    {
        toWrite[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toWrite[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toWrite[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toWrite[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toWrite[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toWrite[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toWrite[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toWrite[i].image = outputs[i];
        toWrite[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 2, toWrite);

    const DlssNr::GuideMatch::Vk::Push push =
        DlssNr::GuideMatch::Vk::MakePush(depthRegion, motionRegion, outWidth, outHeight);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, _layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, DlssNr::GuideMatch::Vk::Groups(outWidth), DlssNr::GuideMatch::Vk::Groups(outHeight), 1);

    // After: the writes visible to whatever reads them next, in whatever stage the model's kernels run.
    VkImageMemoryBarrier written[2] = { toWrite[0], toWrite[1] };
    for (VkImageMemoryBarrier& barrier : written)
    {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 2, written);
}

void DlssNr_GuideMatch_Vk::Destroy()
{
    if (_device != VK_NULL_HANDLE)
    {
        // The sets go with their pool.
        if (_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(_device, _pool, nullptr);
        if (_pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(_device, _pipeline, nullptr);
        if (_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(_device, _layout, nullptr);
        if (_setLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(_device, _setLayout, nullptr);
    }

    Abandon();
}

void DlssNr_GuideMatch_Vk::Abandon() { *this = DlssNr_GuideMatch_Vk {}; }
