#pragma once

// The guide resample on Vulkan: depth and motion vectors point-sampled from the game's regions into two working-size
// images for the NR model below its frame size. docs/dlssnr/design/reduced-scale-guides-vk.md; the D3D12 pass is
// DlssNr_GuideMatch_Dx12, and the shader is the same file, precompile/dlssnr_guides.hlsl, built with -D VK_MODE.
//
// The rules are pure and come first, so tests/nr-vk-guides can hold them without a device. The pass after them is a
// plain bundle of handles: nothing in it logs (it returns its reasons, the route says them) and no destructor touches
// Vulkan. Destroy() frees it once the caller has proven the GPU done with it; Abandon() forgets it on a device that is
// gone. tests/guide-match-vk runs it on a real device.

#include <vulkan/vulkan.h>

#include "DlssNr_GuideMatch.h"

#include <cstdint>
#include <string>

namespace DlssNr::GuideMatch::Vk
{
// The outputs: D3D12's R32_FLOAT and R32G32_FLOAT. Vectors in pixels reach the thousands, where 16-bit float has lost
// the sub-pixel. Both are mandatory storage formats; Unsupported() asks anyway.
constexpr VkFormat kDepthFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kMotionFormat = VK_FORMAT_R32G32_SFLOAT;

// What the route creates every image of its own with (CreateImage in DlssNrFeature_Vk.cpp).
constexpr VkImageUsageFlags kOutputUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

// The shader's group, 8x8x1: 64 invocations, under the 128 every device guarantees.
constexpr uint32_t kGroup = 8;

constexpr uint32_t Groups(uint32_t extent) { return (extent + kGroup - 1) / kGroup; }

// The descriptors are pushed into the command buffer (VK_KHR_push_descriptor), not written into sets the pass keeps:
// what a recording binds is in the recording, however many evaluates one command buffer holds and however often a
// game submits it again. Its four descriptors are under the 32 the extension guarantees (maxPushDescriptors).
// Without the extension on the device the pass is not made: a stable refusal (reduced-scale-guides-vk.md, V-01).
constexpr uint32_t kPushDescriptors = 4;

// The push-constant block: dlssnr_guides.hlsl's GuideParams, the D3D12 cbuffer's twelve scalars in the same order
// (tests/nr-invariants holds the two equal). 48 bytes, within the 128 maxPushConstantsSize guarantees; recorded into
// the command buffer, so no host write can race a pending dispatch's read.
struct Push
{
    uint32_t DepthBaseX;
    uint32_t DepthBaseY;
    uint32_t DepthWidth;
    uint32_t DepthHeight;
    uint32_t MotionBaseX;
    uint32_t MotionBaseY;
    uint32_t MotionWidth;
    uint32_t MotionHeight;
    uint32_t OutWidth;
    uint32_t OutHeight;
    uint32_t Pad0;
    uint32_t Pad1;
};

static_assert(sizeof(Push) == 48 && sizeof(Push) <= 128 && sizeof(Push) % 4 == 0,
              "push constants: a multiple of 4, within the guaranteed 128 bytes");

constexpr Push MakePush(Region depth, Region motion, uint32_t outWidth, uint32_t outHeight)
{
    return Push { depth.baseX,
                  depth.baseY,
                  depth.width,
                  depth.height,
                  motion.baseX,
                  motion.baseY,
                  motion.width,
                  motion.height,
                  outWidth,
                  outHeight,
                  0,
                  0 };
}

// What a format looks like to the shader's Texture2D<float> / Texture2D<float2> read.
struct FormatShape
{
    bool known = false;
    uint32_t components = 0;
    bool floatRead = false; // SFLOAT, UFLOAT, UNORM or SNORM: the shader's float sampled type
    bool depth = false;
    bool stencil = false;
};

constexpr FormatShape Shape(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        return { true, 1, true, true, false };
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return { true, 1, true, true, true };
    case VK_FORMAT_S8_UINT:
        return { true, 1, false, false, true };

    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
        return { true, 1, true, false, false };
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
        return { true, 2, true, false, false };
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R16G16B16_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT:
        return { true, 3, true, false, false };
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return { true, 4, true, false, false };

    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
        return { true, 1, false, false, false };
    case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
        return { true, 2, false, false, false };
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
        return { true, 4, false, false, false };

    default:
        return {};
    }
}

// A 2D view has one layer; VK_REMAINING_ARRAY_LAYERS on a 2D view is one too (VUID-VkImageViewCreateInfo-
// imageViewType-04973). More is an array view, which the shader's non-arrayed image must not read
// (VUID-vkCmdDispatch-viewType-07752).
constexpr bool OneLayer(uint32_t layerCount) { return layerCount <= 1 || layerCount == VK_REMAINING_ARRAY_LAYERS; }

// Whether the game's depth view, as NGX was handed it, may be bound to binding 0: nullptr if so, otherwise why not.
// `optimal` is the device's optimal-tiling features for the view's format.
constexpr const char* DepthViewRefusal(VkFormat format, VkImageAspectFlags aspect, uint32_t layerCount,
                                       VkFormatFeatureFlags optimal)
{
    const FormatShape shape = Shape(format);

    if (!shape.known)
        return "the game's depth view has a format the resample does not know";
    if (!shape.floatRead && !shape.depth)
        return "the game's depth view is an integer format, which the resample's float read must not sample";
    if (shape.depth && shape.stencil && aspect != VK_IMAGE_ASPECT_DEPTH_BIT)
        return "the game's depth view does not name the depth aspect alone (a sampled view of a depth/stencil image "
               "must name one aspect)";
    if (shape.depth && !shape.stencil && aspect != 0 && aspect != VK_IMAGE_ASPECT_DEPTH_BIT)
        return "the game's depth view names an aspect its depth format does not have";
    if (!shape.depth && aspect != 0 && aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return "the game's depth view of a colour format names another aspect than colour";
    if (!OneLayer(layerCount))
        return "the game's depth view has more than one layer";
    if ((optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0)
        return "the device cannot sample the game's depth format";

    return nullptr;
}

// The same for the motion view and binding 1: a float colour format with an x and a y.
constexpr const char* MotionViewRefusal(VkFormat format, VkImageAspectFlags aspect, uint32_t layerCount,
                                        VkFormatFeatureFlags optimal)
{
    const FormatShape shape = Shape(format);

    if (!shape.known)
        return "the game's motion view has a format the resample does not know";
    if (shape.depth || shape.stencil)
        return "the game's motion view is a depth or stencil format";
    if (!shape.floatRead)
        return "the game's motion view is an integer format, which the resample's float read must not sample";
    if (shape.components < 2)
        return "the game's motion view has one component, no y to read";
    if (aspect != 0 && aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return "the game's motion view names another aspect than colour";
    if (!OneLayer(layerCount))
        return "the game's motion view has more than one layer";
    if ((optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0)
        return "the device cannot sample the game's motion format";

    return nullptr;
}

// Whether the regions the route would hand over can be resampled: every texel of the output reads one texel of each,
// so neither may be empty (the shader's `regionExtent - 1` would wrap and read past the image). A game's subrect base
// at the edge of its image leaves one empty once the route has clamped it. nullptr if both have texels.
constexpr const char* RegionRefusal(Region depth, Region motion)
{
    if (depth.width == 0 || depth.height == 0)
        return "the game's depth region is empty";
    if (motion.width == 0 || motion.height == 0)
        return "the game's motion region is empty";

    return nullptr;
}

// Whether the device can hold the outputs: storage and sampled in optimal tiling, and an image of the route's usage at
// the working size (vkGetPhysicalDeviceImageFormatProperties). nullptr if so.
constexpr const char* OutputRefusal(VkFormatFeatureFlags depthOptimal, bool depthImage,
                                    VkFormatFeatureFlags motionOptimal, bool motionImage)
{
    constexpr VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

    if ((depthOptimal & need) != need)
        return "the device has no R32_SFLOAT storage and sampling";
    if ((motionOptimal & need) != need)
        return "the device has no R32G32_SFLOAT storage and sampling";
    if (!depthImage)
        return "the device cannot make an R32_SFLOAT image of the route's usage at the working size";
    if (!motionImage)
        return "the device cannot make an R32G32_SFLOAT image of the route's usage at the working size";

    return nullptr;
}
} // namespace DlssNr::GuideMatch::Vk

class DlssNr_GuideMatch_Vk
{
    VkDevice _device = VK_NULL_HANDLE;
    VkPhysicalDevice _physical = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    PFN_vkCmdPushDescriptorSetKHR _push = nullptr;
    bool _lost = false;

    // The last format asked about for each input and its optimal-tiling features: the game's views keep one format.
    VkFormat _depthFormat = VK_FORMAT_UNDEFINED;
    VkFormatFeatureFlags _depthFeatures = 0;
    VkFormat _motionFormat = VK_FORMAT_UNDEFINED;
    VkFormatFeatureFlags _motionFeatures = 0;

  public:
    // nullptr when this device can hold the outputs at width x height; otherwise why not.
    static const char* Unsupported(VkPhysicalDevice physical, uint32_t width, uint32_t height);

    // The pipeline and its layouts, on `device`, which must have VK_KHR_push_descriptor enabled. False with `why` set,
    // everything made so far freed -- unless the device was lost on the way (Lost()), in which case it is only
    // forgotten. `stable`, when given, says whether the failure is one that trying again will not change (the
    // extension not enabled), as opposed to one that might (an allocation).
    bool Create(VkDevice device, VkPhysicalDevice physical, std::string& why, bool* stable = nullptr);

    bool Ready() const { return _pipeline != VK_NULL_HANDLE; }
    bool Lost() const { return _lost; }
    VkDevice Device() const { return _device; }

    // Whether the game's views may be bound: nullptr if both may, otherwise why not (DepthViewRefusal,
    // MotionViewRefusal, with the device's features for their formats).
    const char* InputRefusal(VkFormat depthFormat, const VkImageSubresourceRange& depthRange, VkFormat motionFormat,
                             const VkImageSubresourceRange& motionRange);

    // Records the resample: both outputs moved from UNDEFINED to GENERAL (their contents discarded: every texel is
    // written), the point sample of the two regions into them, and a barrier that makes the writes visible to whatever
    // reads them next. The inputs are read in `inputLayout` and left in it. Ready() must be true and InputRefusal()
    // must have said nothing for these views; regions must be non-empty and inside their textures.
    void Record(VkCommandBuffer cmd, VkImageView depth, VkImageView motion, VkImageLayout inputLayout,
                const DlssNr::GuideMatch::Region& depthRegion, const DlssNr::GuideMatch::Region& motionRegion,
                VkImage depthOut, VkImageView depthOutView, VkImage motionOut, VkImageView motionOutView,
                uint32_t outWidth, uint32_t outHeight);

    // Frees everything. Only once nothing the GPU runs can still use it: after a device-wide wait, or fences that
    // cover every submission that recorded it.
    void Destroy();

    // Forgets everything without a call: the device is gone, and took them with it.
    void Abandon();
};
