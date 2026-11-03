#include "pch.h"

#include "DlssNr_MotionProbe_Vk.h"
#include "DlssNr_GuideMatch_Vk.h"

#include <dlssnr/DlssNr_VkRecording.h>

#include <cstring>
#include <iterator>
#include <vector>

// The probe's SPIR-V, when the build carries it. Its absence is a supported state, as on D3D12: the route says the
// probe is unavailable once and no verdict forms; nothing else on the route depends on it. See dlssnr_motion_probe.hlsl
// for the command that makes it.
#if __has_include("precompile/DlssNr_MotionProbe_Shader_Vk.h")
#include "precompile/DlssNr_MotionProbe_Shader_Vk.h"
#define DLSSNR_MOTION_PROBE_SPIRV 1
#else
#define DLSSNR_MOTION_PROBE_SPIRV 0
#endif

// The recording registry carries no Vulkan types; the bit it reads Begin's flags for is this one.
static_assert(DlssNr::VkRecording::kSimultaneousUseBit == VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
              "VkRecording::ReuseOf reads VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT");

namespace
{
using namespace DlssNr::MotionProbe::Vk;

constexpr VkDescriptorSetLayoutBinding kBindings[] = {
    { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // gDepth, the game's
    { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // gMotion, the game's
    { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, // gGrid, rgba32f
};
static_assert(std::size(kBindings) == kPushDescriptors, "one pushed descriptor per binding");

VkFormatFeatureFlags OptimalFeatures(VkPhysicalDevice physical, VkFormat format)
{
    VkFormatProperties properties {};
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    return properties.optimalTilingFeatures;
}
} // namespace

bool DlssNr_MotionProbe_Vk::Available() { return DLSSNR_MOTION_PROBE_SPIRV != 0; }

const char* DlssNr_MotionProbe_Vk::Unsupported(VkPhysicalDevice physical)
{
    if (physical == VK_NULL_HANDLE)
        return "no physical device";

    return GridRefusal(OptimalFeatures(physical, kGridFormat));
}

bool DlssNr_MotionProbe_Vk::Create(VkDevice device, VkPhysicalDevice physical, std::string& why)
{
    // Made once: a pass already made is kept, never freed here (nothing proves the GPU done with it). A failed attempt
    // leaves nothing behind, so there is nothing else to clear.
    if (Ready())
        return true;

    Abandon();
    _device = device;
    _physical = physical;

    if (!Available())
    {
        why = "this build carries no probe shader";
        Abandon();
        return false;
    }

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

    // As the guide resample: the entry point is there only with the extension enabled on the device, which the NR
    // device-extension block adds wherever the device offers it.
    _push = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
    if (_push == nullptr)
    {
        why = "VK_KHR_push_descriptor is not enabled on the device";
        Abandon();
        return false;
    }

#if DLSSNR_MOTION_PROBE_SPIRV
    // Every object is created into a local and published only on VK_SUCCESS: a failed vkCreate* leaves its output
    // undefined, and the cleanup in failed() destroys every member that is not null.
    VkDescriptorSetLayoutCreateInfo setInfo { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    setInfo.bindingCount = (uint32_t) std::size(kBindings);
    setInfo.pBindings = kBindings;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    const VkResult setMade = vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout);
    if (setMade == VK_SUCCESS)
        _setLayout = setLayout;
    if (failed(setMade, "the descriptor set layout"))
        return false;

    // One range, the whole block, compute only, from offset 0: a multiple of 4 and within maxPushConstantsSize.
    VkPushConstantRange range {};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = sizeof(Push);

    VkPipelineLayoutCreateInfo layoutInfo { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    const VkResult layoutMade = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout);
    if (layoutMade == VK_SUCCESS)
        _layout = layout;
    if (failed(layoutMade, "the pipeline layout"))
        return false;

    // The generated array is bytes; pCode must be 4-byte aligned words.
    std::vector<uint32_t> words((sizeof(dlssnr_motion_probe_spv) + 3) / 4, 0u);
    std::memcpy(words.data(), dlssnr_motion_probe_spv, sizeof(dlssnr_motion_probe_spv));

    VkShaderModuleCreateInfo moduleInfo { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    moduleInfo.codeSize = sizeof(dlssnr_motion_probe_spv);
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
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult made = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    if (made != VK_ERROR_DEVICE_LOST)
        vkDestroyShaderModule(device, module, nullptr);
    if (made == VK_SUCCESS)
        _pipeline = pipeline;
    if (failed(made, "the compute pipeline"))
        return false;

    return true;
#else
    (void) failed;
    why = "this build carries no probe shader";
    Abandon();
    return false;
#endif
}

const char* DlssNr_MotionProbe_Vk::InputRefusal(VkFormat depthFormat, const VkImageSubresourceRange& depthRange,
                                                VkFormat motionFormat, const VkImageSubresourceRange& motionRange)
{
    if (_physical == VK_NULL_HANDLE)
        return "the probe has no device";

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

    if (const char* why = DlssNr::GuideMatch::Vk::DepthViewRefusal(depthFormat, depthRange.aspectMask,
                                                                   depthRange.layerCount, _depthFeatures))
        return why;

    return DlssNr::GuideMatch::Vk::MotionViewRefusal(motionFormat, motionRange.aspectMask, motionRange.layerCount,
                                                     _motionFeatures);
}

void DlssNr_MotionProbe_Vk::Record(VkCommandBuffer cmd, VkImageView depth, VkImageView motion,
                                   VkImageLayout inputLayout, const DlssNr::GuideMatch::Region& depthRegion,
                                   const DlssNr::GuideMatch::Region& motionRegion, VkImage grid, VkImageView gridView,
                                   VkBuffer readback)
{
    // The game's views in the layout NGX takes them in, as the guide resample binds them; the grid in GENERAL, the one
    // storage images may be in.
    const VkDescriptorImageInfo images[] = {
        { VK_NULL_HANDLE, depth, inputLayout },
        { VK_NULL_HANDLE, motion, inputLayout },
        { VK_NULL_HANDLE, gridView, VK_IMAGE_LAYOUT_GENERAL },
    };

    VkWriteDescriptorSet writes[kPushDescriptors] {};
    for (uint32_t i = 0; i < kPushDescriptors; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = VK_NULL_HANDLE; // ignored for a push
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = kBindings[i].descriptorType;
        writes[i].pImageInfo = &images[i];
    }

    // Before: whatever used this slot's grid last -- the probe and copy of the recording that held the slot before,
    // which retired before the slot was handed out again -- is done before this write, and the contents are discarded
    // (UNDEFINED): every texel is written below. Discarding is also what lets a slot pass between queue families
    // without an ownership transfer (an EXCLUSIVE image taken over that way has undefined contents, which nothing
    // reads).
    VkImageMemoryBarrier toWrite {};
    toWrite.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toWrite.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; // the copy's read needs only the execution dependency
    toWrite.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toWrite.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toWrite.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toWrite.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toWrite.image = grid;
    toWrite.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toWrite);

    const Push push = MakePush(depthRegion, motionRegion);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    _push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, kPushDescriptors, writes);
    vkCmdPushConstants(cmd, _layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, kGroups, kGroups, 1);

    // The grid, written, to the copy.
    VkImageMemoryBarrier toCopy = toWrite;
    toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toCopy.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toCopy);

    // The whole grid, tightly packed: the flat array of floats FromGrid reads.
    VkBufferImageCopy region {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { DlssNr::MotionProbe::kGrid, DlssNr::MotionProbe::kGrid, 1 };
    vkCmdCopyImageToBuffer(cmd, grid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);

    // The copy has to be visible to a host read, and only the host will read it (the exposure meter's barrier).
    VkBufferMemoryBarrier toHost {};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = readback;
    toHost.offset = 0;
    toHost.size = kReadbackBytes;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0,
                         nullptr);
}

void DlssNr_MotionProbe_Vk::Destroy()
{
    if (_device != VK_NULL_HANDLE)
    {
        if (_pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(_device, _pipeline, nullptr);
        if (_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(_device, _layout, nullptr);
        if (_setLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(_device, _setLayout, nullptr);
    }

    Abandon();
}

void DlssNr_MotionProbe_Vk::Abandon() { *this = DlssNr_MotionProbe_Vk {}; }
