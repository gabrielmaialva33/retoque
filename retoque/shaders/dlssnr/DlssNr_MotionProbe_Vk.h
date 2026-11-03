#pragma once

// Zero-motion detection's probe on Vulkan: the game's depth and motion vectors read on a 32x32 grid over their regions,
// copied into a host-visible buffer the after-upscale route reads back without a wait. docs/dlssnr/design/
// zero-motion-detection.md, "Vulkan"; the D3D12 pass is DlssNr_MotionProbe_Dx12, and the shader is the same file,
// precompile/dlssnr_motion_probe.hlsl, built with -D VK_MODE. What the route keeps between evaluates (the slots, their
// retirement, the verdict and whether it may act) is DlssNr_VkMotionProbe.h.
//
// The rules are pure and come first, so tests/nr-vk-zero-motion can hold them without a device. The pass after them is
// a plain bundle of handles, in the shape of DlssNr_GuideMatch_Vk: nothing in it logs (it returns its reasons, the
// route says them) and no destructor touches Vulkan. Destroy() frees it once the caller has proven the GPU done with
// it; Abandon() forgets it on a device that is gone. The grids and readback buffers are the route's, one per slot.

#include <vulkan/vulkan.h>

#include "DlssNr_GuideMatch.h"
#include <dlssnr/DlssNr_MotionProbe.h>

#include <cstdint>
#include <string>

namespace DlssNr::MotionProbe::Vk
{
// The grid: per cell (depth, depth slope, non-zero motion samples, motion samples), D3D12's R32G32B32A32_FLOAT. A
// mandatory storage format; Unsupported() asks anyway, with the copy out of it.
constexpr VkFormat kGridFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

// The readback a grid is copied into, tightly packed: what FromGrid reads.
constexpr VkDeviceSize kReadbackBytes = VkDeviceSize { kGridFloats } * sizeof(float);
static_assert(kReadbackBytes == 16384, "a 32x32 RGBA32F grid");

// The shader's group, 8x8x1, and the dispatch that covers the grid: one thread per cell.
constexpr uint32_t kGroup = 8;
constexpr uint32_t kGroups = (kGrid + kGroup - 1) / kGroup;

// Pushed into the command buffer (VK_KHR_push_descriptor), as the guide resample's are: what a recording binds is in
// the recording. The game's depth and motion at bindings 0 and 1, the grid at 2; under the 32 the extension guarantees.
constexpr uint32_t kPushDescriptors = 3;

// The push-constant block: dlssnr_motion_probe.hlsl's ProbeParams, the D3D12 cbuffer's eight scalars in the same order
// (tests/nr-invariants holds the three equal). 32 bytes, within the 128 maxPushConstantsSize guarantees; recorded into
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
};

static_assert(sizeof(Push) == 32 && sizeof(Push) <= 128 && sizeof(Push) % 4 == 0,
              "push constants: a multiple of 4, within the guaranteed 128 bytes");

constexpr Push MakePush(const GuideMatch::Region& depth, const GuideMatch::Region& motion)
{
    return Push { depth.baseX,  depth.baseY,  depth.width,  depth.height,
                  motion.baseX, motion.baseY, motion.width, motion.height };
}

// What is probed, for the detector: another shape is another buffer, judged afresh. The same fields as the D3D12
// route's (DlssNr_Dx12.cpp, the probe's Shape).
constexpr Shape ShapeOf(const GuideMatch::Region& depth, const GuideMatch::Region& motion, bool motionLowResolution)
{
    Shape shape;
    shape.depthWidth = depth.width;
    shape.depthHeight = depth.height;
    shape.depthBaseX = depth.baseX;
    shape.depthBaseY = depth.baseY;
    shape.motionWidth = motion.width;
    shape.motionHeight = motion.height;
    shape.motionBaseX = motion.baseX;
    shape.motionBaseY = motion.baseY;
    shape.motionLowResolution = motionLowResolution;
    return shape;
}

// Whether the device can hold a grid and copy it out: storage and transfer source in optimal tiling. nullptr if so.
constexpr const char* GridRefusal(VkFormatFeatureFlags optimal)
{
    if ((optimal & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0)
        return "the device has no R32G32B32A32_SFLOAT storage image";
    if ((optimal & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0)
        return "the device cannot copy out of an R32G32B32A32_SFLOAT image";

    return nullptr;
}
} // namespace DlssNr::MotionProbe::Vk

class DlssNr_MotionProbe_Vk
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
    // Whether this build carries the probe's SPIR-V (optional, __has_include, as the D3D12 pass's DXIL is).
    static bool Available();

    // nullptr when this device can hold a grid and copy it out; otherwise why not.
    static const char* Unsupported(VkPhysicalDevice physical);

    // The pipeline and its layouts, on `device`, which must have VK_KHR_push_descriptor enabled. False with `why` set,
    // everything made so far freed -- unless the device was lost on the way (Lost()), in which case it is only
    // forgotten.
    bool Create(VkDevice device, VkPhysicalDevice physical, std::string& why);

    bool Ready() const { return _pipeline != VK_NULL_HANDLE; }
    bool Lost() const { return _lost; }
    VkDevice Device() const { return _device; }

    // Whether the game's views may be bound: nullptr if both may, otherwise why not (the guide resample's rules,
    // DepthViewRefusal and MotionViewRefusal: the probe reads the same types with the same Load).
    const char* InputRefusal(VkFormat depthFormat, const VkImageSubresourceRange& depthRange, VkFormat motionFormat,
                             const VkImageSubresourceRange& motionRange);

    // Records the probe into `grid` and its copy into `readback`: the grid moved from UNDEFINED to GENERAL (its
    // contents discarded: every texel is written), the dispatch over the two regions, the grid moved to TRANSFER_SRC
    // and copied out, and the copy made visible to a host read. The game's views are read in `inputLayout` and left in
    // it. `grid` and `readback` belong to one slot, which belongs to this recording alone. Ready() must be true and
    // InputRefusal() must have said nothing for these views; regions must be non-empty and inside their textures.
    void Record(VkCommandBuffer cmd, VkImageView depth, VkImageView motion, VkImageLayout inputLayout,
                const DlssNr::GuideMatch::Region& depthRegion, const DlssNr::GuideMatch::Region& motionRegion,
                VkImage grid, VkImageView gridView, VkBuffer readback);

    // Frees everything. Only once nothing the GPU runs can still use it.
    void Destroy();

    // Forgets everything without a call: the device is gone, and took them with it.
    void Abandon();
};
