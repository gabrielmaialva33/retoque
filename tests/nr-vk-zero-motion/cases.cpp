// Zero-motion detection on the Vulkan after-upscale route (docs/dlssnr/design/zero-motion-detection.md, "Vulkan"), on
// the host. Compiled with:
//   - the production DlssNr_VkMotionProbe.h, with the real VkMeterReadback::Ring, VkRecording::Registry and
//     DlssNr_MotionProbe.h's Detector;
//   - the production DlssNr_MotionProbe_Vk.cpp (a separate unit, against the real Vulkan headers), whose Vulkan calls
//     land in the fakes below and are recorded;
//   - the route's probe functions and its probe state, extracted verbatim from DlssNrFeature_Vk.cpp by run.py
//     (PRODUCTION_STATE, PRODUCTION_ROUTE).
// No GPU: "the GPU ran the copy" is a grid written into the slot's mapped buffer, "the game reset its command buffer"
// is the registry seeing it. No wait function is defined here: a probe path that called one would not link.

#include <vulkan/vulkan.h>

#include <dlssnr/DlssNr_MotionProbe.h>
#include <dlssnr/DlssNr_VkMeterReadback.h>
#include <dlssnr/DlssNr_VkMotionProbe.h>
#include <dlssnr/DlssNr_VkRecording.h>

#include "DlssNr_GuideMatch.h"
#include "DlssNr_GuideMatch_Vk.h"
#include "DlssNr_MotionProbe_Vk.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// --- Allocation count, for the default-off case ---------------------------------------------------------------------

static bool g_countAllocations = false;
static unsigned g_allocations = 0;
void* operator new(std::size_t size)
{
    if (g_countAllocations)
        ++g_allocations;
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// --- Fake Vulkan ----------------------------------------------------------------------------------------------------

template <class T> T H(uint64_t value) { return reinterpret_cast<T>(static_cast<uintptr_t>(value)); }
template <class T> uint64_t U(T handle) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle)); }

enum class Op
{
    ImageBarrier,
    BufferBarrier,
    Bind,
    PushDescriptors,
    PushConstants,
    Dispatch,
    Copy,
};

struct Command
{
    Op op;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkPipelineStageFlags srcStage = 0, dstStage = 0;
    VkImageMemoryBarrier image {};
    VkBufferMemoryBarrier buffer {};
    std::vector<VkDescriptorImageInfo> descriptors;
    std::vector<VkDescriptorType> types;
    std::vector<uint32_t> push;
    uint32_t groups[3] = {};
    VkImage copySource = VK_NULL_HANDLE;
    VkImageLayout copyLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkBuffer copyTarget = VK_NULL_HANDLE;
    VkBufferImageCopy region {};
};

struct Fake
{
    uint64_t next = 0x1000;
    bool pushDescriptor = true;
    VkResult pipelineResult = VK_SUCCESS;
    VkResult bufferResult = VK_SUCCESS;
    // A failing create writes this into its output first: Vulkan leaves a failed create's output undefined.
    VkResult setLayoutResult = VK_SUCCESS;
    VkResult layoutResult = VK_SUCCESS;
    static constexpr uint64_t kPoison = 0xDEADBEEF00;
    std::vector<uint64_t> made;   // the pass's layouts and pipelines that were created
    std::vector<uint64_t> unmade; // the same kinds, destroyed
    VkFormatFeatureFlags gridFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    VkFormatFeatureFlags inputFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    unsigned created = 0;   // every object made
    unsigned destroyed = 0; // every object destroyed, freed or unmapped
    unsigned images = 0;
    unsigned retiredImages = 0;
    std::vector<VkMemoryPropertyFlags> memoryAsked;
    std::map<uint64_t, std::vector<float>> memory; // mapped backing, per allocation
    std::vector<Command> commands;

    uint64_t Next() { return next += 8; }
} g_fake;

extern "C"
{
    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat format,
                                                                   VkFormatProperties* properties)
    {
        *properties = {};
        properties->optimalTilingFeatures =
            format == VK_FORMAT_R32G32B32A32_SFLOAT ? g_fake.gridFeatures : g_fake.inputFeatures;
    }

    VKAPI_ATTR void VKAPI_CALL FakePushDescriptors(VkCommandBuffer cmd, VkPipelineBindPoint, VkPipelineLayout, uint32_t,
                                                   uint32_t count, const VkWriteDescriptorSet* writes)
    {
        Command c { Op::PushDescriptors, cmd };
        for (uint32_t i = 0; i < count; ++i)
        {
            c.descriptors.push_back(*writes[i].pImageInfo);
            c.types.push_back(writes[i].descriptorType);
        }
        g_fake.commands.push_back(std::move(c));
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char* name)
    {
        if (g_fake.pushDescriptor && std::strcmp(name, "vkCmdPushDescriptorSetKHR") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(&FakePushDescriptors);
        return nullptr;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo* info,
                                                               const VkAllocationCallbacks*,
                                                               VkDescriptorSetLayout* layout)
    {
        if (info->bindingCount != 3 || (info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) == 0)
            throw std::runtime_error("the probe's set layout must be three pushed bindings");
        if (g_fake.setLayoutResult != VK_SUCCESS)
        {
            *layout = H<VkDescriptorSetLayout>(Fake::kPoison + 1);
            return g_fake.setLayoutResult;
        }
        ++g_fake.created;
        *layout = H<VkDescriptorSetLayout>(g_fake.Next());
        g_fake.made.push_back(U(*layout));
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo* info,
                                                          const VkAllocationCallbacks*, VkPipelineLayout* layout)
    {
        if (info->pushConstantRangeCount != 1 || info->pPushConstantRanges[0].size != 32 ||
            info->pPushConstantRanges[0].offset != 0)
            throw std::runtime_error("the probe's push range must be the 32-byte block from offset 0");
        if (g_fake.layoutResult != VK_SUCCESS)
        {
            *layout = H<VkPipelineLayout>(Fake::kPoison + 2);
            return g_fake.layoutResult;
        }
        ++g_fake.created;
        *layout = H<VkPipelineLayout>(g_fake.Next());
        g_fake.made.push_back(U(*layout));
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo* info,
                                                        const VkAllocationCallbacks*, VkShaderModule* module)
    {
        if (info->codeSize % 4 != 0 || info->pCode[0] != 0x07230203u)
            throw std::runtime_error("the probe's module must be whole SPIR-V words");
        ++g_fake.created;
        *module = H<VkShaderModule>(g_fake.Next());
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t,
                                                            const VkComputePipelineCreateInfo*,
                                                            const VkAllocationCallbacks*, VkPipeline* pipeline)
    {
        if (g_fake.pipelineResult != VK_SUCCESS)
        {
            *pipeline = H<VkPipeline>(Fake::kPoison + 3);
            return g_fake.pipelineResult;
        }
        ++g_fake.created;
        *pipeline = H<VkPipeline>(g_fake.Next());
        g_fake.made.push_back(U(*pipeline));
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*)
    {
        ++g_fake.destroyed;
    }
    VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline pipeline, const VkAllocationCallbacks*)
    {
        ++g_fake.destroyed;
        g_fake.unmade.push_back(U(pipeline));
    }
    VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice, VkPipelineLayout layout, const VkAllocationCallbacks*)
    {
        ++g_fake.destroyed;
        g_fake.unmade.push_back(U(layout));
    }
    VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout layout,
                                                            const VkAllocationCallbacks*)
    {
        ++g_fake.destroyed;
        g_fake.unmade.push_back(U(layout));
    }

    VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src,
                                                    VkPipelineStageFlags dst, VkDependencyFlags, uint32_t,
                                                    const VkMemoryBarrier*, uint32_t bufferCount,
                                                    const VkBufferMemoryBarrier* buffers, uint32_t imageCount,
                                                    const VkImageMemoryBarrier* images)
    {
        for (uint32_t i = 0; i < imageCount; ++i)
        {
            Command c { Op::ImageBarrier, cmd, src, dst };
            c.image = images[i];
            g_fake.commands.push_back(c);
        }
        for (uint32_t i = 0; i < bufferCount; ++i)
        {
            Command c { Op::BufferBarrier, cmd, src, dst };
            c.buffer = buffers[i];
            g_fake.commands.push_back(c);
        }
    }

    VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint, VkPipeline)
    {
        g_fake.commands.push_back(Command { Op::Bind, cmd });
    }

    VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout, VkShaderStageFlags,
                                                  uint32_t offset, uint32_t size, const void* values)
    {
        if (offset != 0 || size != 32)
            throw std::runtime_error("the probe pushes its whole 32-byte block");
        Command c { Op::PushConstants, cmd };
        c.push.resize(size / 4);
        std::memcpy(c.push.data(), values, size);
        g_fake.commands.push_back(c);
    }

    VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z)
    {
        Command c { Op::Dispatch, cmd };
        c.groups[0] = x;
        c.groups[1] = y;
        c.groups[2] = z;
        g_fake.commands.push_back(c);
    }

    VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
                                                      VkBuffer buffer, uint32_t count, const VkBufferImageCopy* regions)
    {
        if (count != 1)
            throw std::runtime_error("one copy region");
        Command c { Op::Copy, cmd };
        c.copySource = image;
        c.copyLayout = layout;
        c.copyTarget = buffer;
        c.region = regions[0];
        g_fake.commands.push_back(c);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo* info,
                                                  const VkAllocationCallbacks*, VkBuffer* buffer)
    {
        if (g_fake.bufferResult != VK_SUCCESS)
            return g_fake.bufferResult;
        if (info->size != 16384 || info->usage != VK_BUFFER_USAGE_TRANSFER_DST_BIT ||
            info->sharingMode != VK_SHARING_MODE_EXCLUSIVE)
            throw std::runtime_error("a readback is 16 KB, a copy target, exclusive");
        ++g_fake.created;
        *buffer = H<VkBuffer>(g_fake.Next());
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements* requirements)
    {
        *requirements = {};
        requirements->size = 16384;
        requirements->alignment = 256;
        requirements->memoryTypeBits = 0x3;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info,
                                                    const VkAllocationCallbacks*, VkDeviceMemory* memory)
    {
        ++g_fake.created;
        *memory = H<VkDeviceMemory>(g_fake.Next());
        g_fake.memory[U(*memory)].assign(info->allocationSize / sizeof(float), 0.0f);
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize)
    {
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize, VkDeviceSize size,
                                               VkMemoryMapFlags, void** mapped)
    {
        if (size != 16384)
            throw std::runtime_error("a readback maps its 16 KB");
        *mapped = g_fake.memory.at(U(memory)).data();
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory) { ++g_fake.destroyed; }
    VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) { ++g_fake.destroyed; }
    VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*)
    {
        ++g_fake.destroyed;
        g_fake.memory.erase(U(memory));
    }
}

namespace DeferredRelease
{
std::vector<std::function<void()>> pending;
void Enqueue(std::function<void()> release, uint64_t) { pending.push_back(std::move(release)); }
void Drain()
{
    auto releases = std::move(pending);
    pending.clear();
    for (auto& release : releases)
        release();
}
} // namespace DeferredRelease

// --- The route's surroundings ---------------------------------------------------------------------------------------

std::vector<std::string> g_lines;
#define LOG_INFO(...) g_lines.push_back(std::format(__VA_ARGS__))

enum NVSDK_NGX_Resource_VK_Type
{
    NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW,
    NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER,
};
struct NVSDK_NGX_ImageViewInfo_VK
{
    VkImageView ImageView = VK_NULL_HANDLE;
    VkImage Image = VK_NULL_HANDLE;
    VkImageSubresourceRange SubresourceRange {};
    VkFormat Format = VK_FORMAT_UNDEFINED;
    unsigned int Width = 0;
    unsigned int Height = 0;
};
struct NVSDK_NGX_Resource_VK
{
    union
    {
        NVSDK_NGX_ImageViewInfo_VK ImageViewInfo;
    } Resource {};
    NVSDK_NGX_Resource_VK_Type Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    bool ReadWrite = false;
};

namespace DlssNr
{
struct OwnedImage
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    bool Valid() const { return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE; }
};

struct State
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

    // PRODUCTION_STATE
} g_vk;

bool CreateImageOn(VkDevice, VkPhysicalDevice, OwnedImage& img, uint32_t width, uint32_t height, VkFormat format, bool,
                   bool* = nullptr, bool storage = true)
{
    if (width != 32 || height != 32 || format != VK_FORMAT_R32G32B32A32_SFLOAT || !storage)
        throw std::runtime_error("a grid is a 32x32 RGBA32F storage image");
    ++g_fake.created;
    ++g_fake.images;
    img.image = H<VkImage>(g_fake.Next());
    img.view = H<VkImageView>(g_fake.Next());
    return true;
}

uint32_t FindMemoryTypeIndex(uint32_t, VkMemoryPropertyFlags properties)
{
    g_fake.memoryAsked.push_back(properties);
    return (properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0 ? UINT32_MAX : 1;
}

void RetireImage(OwnedImage& img)
{
    if (img.image != VK_NULL_HANDLE)
        ++g_fake.retiredImages;
    img = OwnedImage {};
}

// PRODUCTION_ROUTE

// --- Fixture ---------------------------------------------------------------------------------------------------------

using MotionProbe::Event;
using VkRecording::Recordings;
using VkRecording::Reuse;

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

constexpr uint64_t kPool = 0x77;
const VkDevice kDeviceA = H<VkDevice>(0xA0);
const VkDevice kDeviceB = H<VkDevice>(0xB0);
const VkPhysicalDevice kPhysical = H<VkPhysicalDevice>(0x70);

struct Views
{
    NVSDK_NGX_Resource_VK depth;
    NVSDK_NGX_Resource_VK motion;
    GuideMatch::Region depthRegion { 8, 4, 1280, 720 };
    GuideMatch::Region motionRegion { 0, 0, 1280, 720 };

    Views()
    {
        depth.Resource.ImageViewInfo.ImageView = H<VkImageView>(0xD1);
        depth.Resource.ImageViewInfo.Format = VK_FORMAT_D32_SFLOAT;
        depth.Resource.ImageViewInfo.SubresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        motion.Resource.ImageViewInfo.ImageView = H<VkImageView>(0xE1);
        motion.Resource.ImageViewInfo.Format = VK_FORMAT_R16G16_SFLOAT;
        motion.Resource.ImageViewInfo.SubresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    }
};

// One evaluate as the route takes it, in the route's order (run.py holds DlssNrFeature_Vk.cpp to the same order):
// readbacks consumed on the same device, a device change forgetting the old one's objects (ShutdownVk(false)), the
// plan, the policy reset, the reservation after the generation pin, and the probe recorded. The game's recording ends
// after it; nothing here resets it.
struct Evaluated
{
    MotionProbePlan plan;
    bool policyReset = false;
    std::optional<VkMeterReadback::Reservation> reservation;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

Evaluated Evaluate(VkDevice device, uint64_t buffer, const Views& views = Views {}, Reuse reuse = Reuse::Serial,
                   bool gameReset = false)
{
    Evaluated e;
    e.cmd = H<VkCommandBuffer>(buffer);
    if (g_vk.device == device)
        ConsumeMotionProbe(true);
    if (g_vk.device != device)
    {
        ForgetMotionProbe();
        g_vk.device = device;
        g_vk.physicalDevice = kPhysical;
    }

    auto& registry = Recordings();
    registry.Allocate(U(device), kPool, { buffer });
    registry.Begin(buffer, reuse);
    const auto ticket = registry.Current(U(device), buffer);
    Check(ticket.has_value(), "the fixture's recording must be open");

    e.plan = PrepareMotionProbe(*ticket, views.depth, views.motion, views.depthRegion, views.motionRegion, false);
    e.policyReset = MotionProbePolicyReset(device, e.plan, true, gameReset);
    if (e.plan.due)
        e.reservation = ReserveMotionProbe(*ticket, e.plan);
    if (e.reservation)
    {
        auto& probe = g_vk.motionProbe;
        const uint32_t slot = e.reservation->slot;
        probe.pass.Record(e.cmd, views.depth.Resource.ImageViewInfo.ImageView,
                          views.motion.Resource.ImageViewInfo.ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          views.depthRegion, views.motionRegion, probe.grid[slot].image, probe.grid[slot].view,
                          probe.readback[slot]);
    }
    e.reservation.reset(); // the route's copy of the pin ends with the evaluate; the recording keeps its own
    registry.End(buffer);
    return e;
}

// Evaluates on a scratch recording of their own until the next one is due; returns how many ran. Any policy reset on
// the way is or-ed into `reset`.
unsigned UntilDue(VkDevice device, bool* reset = nullptr, const Views& views = Views {})
{
    unsigned ran = 0;
    while (!MotionProbe::Due(g_vk.motionProbe.evaluates))
    {
        const auto e = Evaluate(device, 0x9999, views);
        if (reset != nullptr)
            *reset = *reset || e.policyReset;
        ++ran;
    }
    return ran;
}

// The due evaluate, on its own recording `buffer`: the slot it got, or -1.
struct Probed
{
    Evaluated e;
    int slot = -1;
};

Probed ProbeOn(VkDevice device, uint64_t buffer, const Views& views = Views {}, Reuse reuse = Reuse::Serial)
{
    UntilDue(device, nullptr, views);
    Probed p;
    const auto before = g_fake.commands.size();
    p.e = Evaluate(device, buffer, views, reuse);
    for (auto i = before; i < g_fake.commands.size(); ++i)
        if (g_fake.commands[i].op == Op::Copy)
            for (uint32_t slot = 0; slot < VkMotionProbe::kSlots; ++slot)
                if (g_vk.motionProbe.readback[slot] == g_fake.commands[i].copyTarget)
                    p.slot = static_cast<int>(slot);
    return p;
}

// A grid as dlssnr_motion_probe.hlsl writes it: a plane receding upward, moved `forward` depth units along the view,
// and `nonZero` non-zero motion samples per cell.
using Grid = std::array<float, MotionProbe::kGridFloats>;
Grid MakeGrid(double forward, unsigned nonZero = 0)
{
    Grid grid {};
    for (unsigned y = 0; y < MotionProbe::kGrid; ++y)
        for (unsigned x = 0; x < MotionProbe::kGrid; ++x)
        {
            float* t = &grid[(y * MotionProbe::kGrid + x) * 4];
            const double px = (x + 0.5) * 40.0, py = (y + 0.5) * 40.0;
            t[0] = static_cast<float>(10.0 + 0.02 * py + 0.005 * px + forward);
            t[1] = 0.02f;
            t[2] = static_cast<float>(nonZero);
            t[3] = static_cast<float>(MotionProbe::kMotionTaps * MotionProbe::kMotionTaps);
        }
    return grid;
}

// The GPU ran the recording's copy into its slot.
void Execute(int slot, const Grid& grid)
{
    Check(slot >= 0, "the probe must have been recorded");
    std::memcpy(g_vk.motionProbe.mapped[slot], grid.data(), sizeof(grid));
}

// The game reset its command buffer: the registry sees it, and the recording's pins retire.
void Retire(uint64_t buffer) { Recordings().Reset(buffer); }

// Events said since `from`, as the route logs them.
unsigned Said(const std::string& needle, size_t from = 0)
{
    unsigned count = 0;
    for (size_t i = from; i < g_lines.size(); ++i)
        count += g_lines[i].find(needle) != std::string::npos;
    return count;
}

void Fresh()
{
    ForgetMotionProbe();
    Recordings().ForgetDevice(U(kDeviceA));
    Recordings().ForgetDevice(U(kDeviceB));
    DeferredRelease::pending.clear();
    g_vk.device = VK_NULL_HANDLE;
    g_vk.physicalDevice = VK_NULL_HANDLE;
    g_vk.motionProbe.evaluates = 0;
    g_vk.motionProbe.refused = 0;
    g_vk.motionProbe.policyResets = 0;
    g_fake = Fake {};
    g_lines.clear();
}

// Baseline, then two pans of an all-zero field: Dead on the third. Each probe on its own recording, retired before the
// next is consumed. Returns the events.
std::vector<Event> FormDeadOn(VkDevice device, uint64_t firstBuffer)
{
    std::vector<Event> events;
    for (unsigned i = 0; i < 3; ++i)
    {
        const auto p = ProbeOn(device, firstBuffer + i);
        Execute(p.slot, MakeGrid(static_cast<double>(i)));
        Retire(firstBuffer + i);
        const auto before = g_lines.size();
        ConsumeMotionProbe(true);
        events.push_back(Said("read all zero", before) ? Event::Dead : Event::None);
    }
    return events;
}

// --- Cases ----------------------------------------------------------------------------------------------------------

void PureRules()
{
    using namespace MotionProbe::Vk;
    const GuideMatch::Region depth { 1, 2, 3, 4 }, motion { 5, 6, 7, 8 };
    const Push push = MakePush(depth, motion);
    const uint32_t expected[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    Check(std::memcmp(&push, expected, sizeof(expected)) == 0, "the push block is the D3D12 cbuffer's order");
    const auto shape = ShapeOf(depth, motion, true);
    Check(shape.depthBaseX == 1 && shape.depthBaseY == 2 && shape.depthWidth == 3 && shape.depthHeight == 4 &&
              shape.motionBaseX == 5 && shape.motionBaseY == 6 && shape.motionWidth == 7 && shape.motionHeight == 8 &&
              shape.motionLowResolution,
          "the shape is the two regions and the low-resolution flag");
    Check(kGroups * kGroup >= MotionProbe::kGrid && (kGroups - 1) * kGroup < MotionProbe::kGrid,
          "the dispatch covers the grid, one group no more");
    Check(kReadbackBytes == VkMotionProbe::kGridBytes, "the readback is the CPU's grid");
    Check(VkMotionProbe::RecordingRefusal(Reuse::Serial) == nullptr &&
              VkMotionProbe::RecordingRefusal(Reuse::Simultaneous) != nullptr &&
              VkMotionProbe::RecordingRefusal(Reuse::Unknown) != nullptr,
          "only a recording that runs one execution at a time carries a probe");
    Check(VkRecording::ReuseOf(false, 0) == Reuse::Unknown && VkRecording::ReuseOf(true, 0) == Reuse::Serial &&
              VkRecording::ReuseOf(true, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) == Reuse::Simultaneous &&
              VkRecording::ReuseOf(true, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) == Reuse::Serial,
          "Begin's flags read as the registry's Reuse");
    Check(GridRefusal(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == nullptr &&
              GridRefusal(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != nullptr &&
              GridRefusal(VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != nullptr,
          "a grid needs storage and a copy out");
}

// What one probe records, in order, and nothing on the game's images.
void RecordedCommands()
{
    const Views views;
    const auto p = ProbeOn(kDeviceA, 10, views);
    Check(p.slot >= 0, "a due evaluate on a serial recording records a probe");
    const auto& probe = g_vk.motionProbe;
    const VkImage grid = probe.grid[p.slot].image;
    const VkBuffer readback = probe.readback[p.slot];

    std::vector<Command> stream;
    for (const auto& c : g_fake.commands)
        if (c.cmd == p.e.cmd)
            stream.push_back(c);
    Check(stream.size() == 8, "barrier, bind, descriptors, constants, dispatch, barrier, copy, host barrier: no more");
    const Op order[] = { Op::ImageBarrier, Op::Bind, Op::PushDescriptors, Op::PushConstants, Op::Dispatch,
                         Op::ImageBarrier, Op::Copy, Op::BufferBarrier };
    for (unsigned i = 0; i < 8; ++i)
        Check(stream[i].op == order[i], "the probe's commands in their order");

    const auto& toWrite = stream[0].image;
    Check(toWrite.image == grid && toWrite.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
              toWrite.newLayout == VK_IMAGE_LAYOUT_GENERAL &&
              stream[0].dstStage == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT &&
              toWrite.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED,
          "the slot's grid starts from UNDEFINED in every recording: its contents are discarded");
    Check(stream[2].descriptors.size() == 3 && stream[2].descriptors[0].imageView == H<VkImageView>(0xD1) &&
              stream[2].descriptors[1].imageView == H<VkImageView>(0xE1) &&
              stream[2].descriptors[0].imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
              stream[2].descriptors[1].imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
              stream[2].types[0] == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
              stream[2].types[1] == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
              stream[2].types[2] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
              stream[2].descriptors[2].imageView == probe.grid[p.slot].view &&
              stream[2].descriptors[2].imageLayout == VK_IMAGE_LAYOUT_GENERAL,
          "the game's views read in SHADER_READ_ONLY_OPTIMAL, the slot's grid written in GENERAL");
    const uint32_t push[8] = { 8, 4, 1280, 720, 0, 0, 1280, 720 };
    Check(std::equal(stream[3].push.begin(), stream[3].push.end(), push), "the push block is the game's regions");
    Check(stream[4].groups[0] == 4 && stream[4].groups[1] == 4 && stream[4].groups[2] == 1, "4x4 groups of 8x8");
    Check(stream[5].image.image == grid && stream[5].image.oldLayout == VK_IMAGE_LAYOUT_GENERAL &&
              stream[5].image.newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
              stream[5].image.srcAccessMask == VK_ACCESS_SHADER_WRITE_BIT &&
              stream[5].image.dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT,
          "the written grid to the copy");
    Check(stream[6].copySource == grid && stream[6].copyTarget == readback &&
              stream[6].copyLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && stream[6].region.bufferRowLength == 0 &&
              stream[6].region.imageExtent.width == 32 && stream[6].region.imageExtent.height == 32,
          "the whole grid into the slot's own buffer, tightly packed");
    const auto& host = stream[7];
    Check(host.buffer.buffer == readback && host.buffer.srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT &&
              host.buffer.dstAccessMask == VK_ACCESS_HOST_READ_BIT && host.srcStage == VK_PIPELINE_STAGE_TRANSFER_BIT &&
              host.dstStage == VK_PIPELINE_STAGE_HOST_BIT && host.buffer.size == 16384,
          "the copy made visible to a host read");
    for (const auto& c : g_fake.commands)
        Check(c.op != Op::ImageBarrier || (c.image.image != H<VkImage>(0xD1) && c.image.image != H<VkImage>(0xE1)),
              "no barrier on the game's images");
    Check(g_fake.memoryAsked.size() >= 2 && (g_fake.memoryAsked[0] & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0 &&
              (g_fake.memoryAsked[1] & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0,
          "a cached coherent readback is asked for first, a coherent one is the fallback");
    Check(Said("motion probe: up") == 1, "the probe says once that it is up");
    Retire(10);
}

// P3: a baseline and two pans of an all-zero field: None, None, Dead. One non-zero sample then: Alive. The policy reset
// follows the verdict and leaves a reset the game asked for to the game.
void BaselineTwoPansThenAlive()
{
    const auto events = FormDeadOn(kDeviceA, 20);
    Check(events.size() == 3 && events[0] == Event::None && events[1] == Event::None && events[2] == Event::Dead,
          "a baseline and two moving comparisons: None, None, Dead");
    Check(g_vk.motionProbe.readbacks.Detector().Dead(), "the verdict holds");

    const auto next = Evaluate(kDeviceA, 0x9999);
    Check(next.policyReset, "a dead field drops the model's history on the next evaluate");
    const auto gameAsked = Evaluate(kDeviceA, 0x9999, Views {}, Reuse::Serial, true);
    Check(!gameAsked.policyReset, "a reset the game asked for stays the game's");

    const auto p = ProbeOn(kDeviceA, 30);
    Execute(p.slot, MakeGrid(3.0, 1));
    Retire(30);
    const auto before = g_lines.size();
    ConsumeMotionProbe(true);
    Check(Said("carry motion again", before) == 1 && !g_vk.motionProbe.readbacks.Detector().Dead(),
          "one non-zero sample: Alive");
    Check(!Evaluate(kDeviceA, 0x9999).policyReset, "no policy reset once the field carries motion");
}

// P3, the sentinel: a slot whose buffer still holds an earlier, valid grid is handed to a recording that retires
// without executing. Read without the sentinel, that old grid would be fed as a new probe -- here, compared with the
// newest one fed, it would read as a moving all-zero field and form a false verdict. With it, nothing is fed.
void SentinelOverOldValidGrid()
{
    // Two recordings alive at once: slot 0 holds the baseline, slot 1 the first pan.
    const auto r1 = ProbeOn(kDeviceA, 40);
    const auto r2 = ProbeOn(kDeviceA, 41);
    Check(r1.slot == 0 && r2.slot == 1, "two live recordings hold two slots");
    Execute(r1.slot, MakeGrid(0.0));
    Execute(r2.slot, MakeGrid(1.0));
    Retire(40);
    Retire(41);
    ConsumeMotionProbe(true);
    Check(g_vk.motionProbe.readbacks.Detector().Streak() == 1 && g_vk.motionProbe.readbacks.Fed() == 2,
          "baseline and one moving reading fed, in order");

    // Slot 0 still holds the baseline's valid grid; the next recording gets it and is discarded unexecuted.
    const Grid old = MakeGrid(0.0);
    Check(std::memcmp(g_vk.motionProbe.mapped[0], old.data(), sizeof(old)) == 0,
          "precondition: slot 0's buffer holds the old valid grid");
    const auto r3 = ProbeOn(kDeviceA, 42);
    Check(r3.slot == 0, "the discarded recording reuses slot 0");
    Retire(42); // reset without ever being submitted
    const auto before = g_lines.size();
    ConsumeMotionProbe(true);
    Check(g_vk.motionProbe.readbacks.Fed() == 2, "a recording that never executed feeds nothing");
    Check(!g_vk.motionProbe.readbacks.Detector().Dead() && Said("read all zero", before) == 0,
          "an old grid must not be taken for a new probe: no verdict");
    Check(!Evaluate(kDeviceA, 0x9999).policyReset, "and no policy reset");
}

// Retirement out of order: the probes are fed in the order they were recorded; a slot recorded before the newest fed
// one is freed unread.
void OutOfOrderRetirement()
{
    const auto b0 = ProbeOn(kDeviceA, 50);
    Execute(b0.slot, MakeGrid(0.0));
    const auto p1 = ProbeOn(kDeviceA, 51); // slot 1
    Execute(p1.slot, MakeGrid(1.0));
    Retire(50);
    ConsumeMotionProbe(true);              // the baseline; slot 0 free again
    const auto p2 = ProbeOn(kDeviceA, 52); // slot 0, recorded after p1
    Execute(p2.slot, MakeGrid(2.0));
    Check(p1.slot == 1 && p2.slot == 0, "the later probe holds the lower slot");
    Retire(52);
    Retire(51);
    const auto before = g_lines.size();
    ConsumeMotionProbe(true);
    Check(Said("read all zero", before) == 1 && g_vk.motionProbe.readbacks.Detector().Dead(),
          "fed in recording order (the pan before the later pan), the verdict forms");

    // Recorded first, retired last: dropped unread.
    const auto early = ProbeOn(kDeviceA, 53);
    const auto late = ProbeOn(kDeviceA, 54);
    Execute(early.slot, MakeGrid(3.0, 5));
    Execute(late.slot, MakeGrid(4.0));
    Retire(54);
    ConsumeMotionProbe(true);
    const auto fed = g_vk.motionProbe.readbacks.Fed();
    Retire(53);
    ConsumeMotionProbe(true);
    Check(g_vk.motionProbe.readbacks.Fed() == fed && g_vk.motionProbe.readbacks.Detector().Dead(),
          "a probe older than the newest fed is dropped unread (its motion does not end the verdict)");
}

// Saturation and retention: four held recordings, a fifth due probe skipped without waiting; elapsed evaluates and an
// executable recording kept for resubmission never free a slot; a verdict whose probes stop coming loses authority.
void SaturationAndStaleness()
{
    FormDeadOn(kDeviceA, 60); // probes at 0, 8 and 16
    Check(Evaluate(kDeviceA, 0x9999).policyReset, "the verdict acts while it is fresh");
    for (uint64_t buffer = 70; buffer < 74; ++buffer)
        Check(ProbeOn(kDeviceA, buffer).slot >= 0, "four recordings hold the four slots");
    const auto fifth = ProbeOn(kDeviceA, 74);
    Check(fifth.slot < 0 && Said("every readback is still held") == 1, "the fifth is skipped, said once");
    const auto sixth = ProbeOn(kDeviceA, 75);
    Check(sixth.slot < 0 && Said("every readback is still held") == 1, "and only once");

    // Evaluates passing free no slot; up to kStaleEvaluates past the newest probe fed the verdict still acts.
    const uint64_t newest = 16;
    while (g_vk.motionProbe.evaluates <= newest + VkMotionProbe::kStaleEvaluates)
        Check(Evaluate(kDeviceA, 0x9999).policyReset, "a verdict acts while its newest probe is recent enough");
    Check(ProbeOn(kDeviceA, 76).slot < 0, "elapsed evaluates free no slot");

    // Past it, the verdict stops acting and is dropped, said once.
    Check(!g_vk.motionProbe.readbacks.Detector().Dead() && Said("verdict on the game's motion vectors is dropped") == 1,
          "a verdict no probe has been read back for loses its authority");
    Check(!Evaluate(kDeviceA, 0x9999).policyReset, "and drops no more history");

    // Executable resubmission: End without reset keeps the slot until the game resets it.
    for (uint64_t buffer = 70; buffer < 74; ++buffer)
        Recordings().End(buffer);
    ConsumeMotionProbe(true);
    Check(ProbeOn(kDeviceA, 80).slot < 0, "an ended, resubmittable recording keeps its slot");
    for (uint64_t buffer = 70; buffer < 74; ++buffer)
        Retire(buffer);
    ConsumeMotionProbe(true);
    Check(ProbeOn(kDeviceA, 81).slot >= 0, "reset, the slots come back");
    Retire(81);
}

// P2b, required case: Dead on device A; the game moves to device B, where the probe stably cannot run. No policy reset
// on B, ever, without new evidence; the refusal counted and said once.
void DeadOnAThenStableFailureOnB()
{
    FormDeadOn(kDeviceA, 100);
    Check(Evaluate(kDeviceA, 0x9999).policyReset, "precondition: the verdict acts on A");

    // Provenance alone: asked about B before anything else happened, the verdict formed on A does not act.
    MotionProbePlan observed;
    observed.observable = true;
    observed.seq = g_vk.motionProbe.evaluates;
    Check(!MotionProbePolicyReset(kDeviceB, observed, true, false), "a verdict formed on A has no authority on B");

    // The device change (ShutdownVk(false) -> ForgetMotionProbe), then B cannot build the probe: no push descriptors.
    g_fake.pushDescriptor = false;
    const auto before = g_lines.size();
    const auto refusedBefore = g_vk.motionProbe.refused;
    bool reset = false;
    for (int i = 0; i < 40; ++i)
    {
        const auto e = Evaluate(kDeviceB, 0x9999);
        reset = reset || e.policyReset;
        Check(!e.plan.observable && !e.plan.due, "B cannot be observed");
    }
    Check(!reset, "Dead on A, stable failure on B: no policy reset on B");
    Check(!g_vk.motionProbe.readbacks.Detector().Dead(), "the verdict is gone with A");
    Check(Said("verdict on the game's motion vectors is dropped (the device it was read on is gone)", before) == 1,
          "the drop is said");
    Check(g_vk.motionProbe.refused == refusedBefore + 40 && Said("VK_KHR_push_descriptor", before) == 1,
          "every refused evaluate counted, the reason said once");
}

// P2b, same device: the game's views become unreadable to the probe; the verdict is dropped and does not come back when
// they are readable again until new probes form it.
void RefusalDropsTheVerdict()
{
    FormDeadOn(kDeviceA, 110);
    Views refused;
    refused.motion.Resource.ImageViewInfo.Format = VK_FORMAT_R16G16_UINT;
    const auto e = Evaluate(kDeviceA, 0x9999, refused);
    Check(!e.plan.observable && !e.policyReset, "a refused frame is not observed and drops no history");
    Check(!Evaluate(kDeviceA, 0x9999).policyReset && !g_vk.motionProbe.readbacks.Detector().Dead(),
          "readable again: the old verdict does not come back by itself");
    Check(Said("integer format") == 1, "the refusal is said once");
}

// P2a: a recording that may run twice at once over the same grid, or whose Begin was not seen with its flags, gets no
// slot; counted, said once per reason. A serial one does.
void RecordingRefusals()
{
    for (int i = 0; i < 3; ++i)
    {
        const auto p = ProbeOn(kDeviceA, 120 + i, Views {}, Reuse::Simultaneous);
        Check(p.slot < 0 && !p.e.plan.observable, "a SIMULTANEOUS_USE recording carries no probe");
    }
    for (int i = 0; i < 3; ++i)
        Check(ProbeOn(kDeviceA, 130 + i, Views {}, Reuse::Unknown).slot < 0, "an unobserved Begin carries no probe");
    Check(Said("SIMULTANEOUS_USE_BIT") == 1 && Said("were not observed") == 1, "each reason said once");
    Check(g_vk.motionProbe.refused == 6, "every refused evaluate counted");
    const auto ok = ProbeOn(kDeviceA, 140, Views {}, Reuse::Serial);
    Check(ok.slot >= 0, "a serial recording carries one");
    Retire(140);
    for (int i = 0; i < 3; ++i)
    {
        Retire(120 + i);
        Retire(130 + i);
    }
}

// P2a: two recordings alive at once write two grids and two buffers; one recording executed twice in turn writes its
// own slot twice and is read once, after it retires (which execution's grid that is, is the declared limit).
void SlotsAreExclusive()
{
    const auto a = ProbeOn(kDeviceA, 150);
    const auto b = ProbeOn(kDeviceA, 151);
    Check(a.slot >= 0 && b.slot >= 0 && a.slot != b.slot, "two live recordings, two slots");
    const auto& probe = g_vk.motionProbe;
    Check(probe.grid[a.slot].image != probe.grid[b.slot].image && probe.readback[a.slot] != probe.readback[b.slot],
          "each slot has a grid and a buffer of its own");
    std::vector<VkImage> written;
    for (const auto& c : g_fake.commands)
        if (c.op == Op::Copy)
            written.push_back(c.copySource);
    Check(written.size() == 2 && written[0] != written[1], "the two recordings copy from different grids");

    Execute(a.slot, MakeGrid(0.0));
    Execute(a.slot, MakeGrid(0.5)); // submitted again after the first execution completed
    const auto fed = probe.readbacks.Fed();
    ConsumeMotionProbe(true);
    Check(probe.readbacks.Fed() == fed, "nothing is read while the recording can still run");
    Retire(150);
    Retire(151);
    ConsumeMotionProbe(true);
    Check(probe.readbacks.Fed() == fed + 1, "read once, after the reset (b never executed: sentinel, not fed)");
}

// The shape the probe was reserved with is the shape fed: a change of region while dead says Reshaped.
void ShapeAtReservation()
{
    FormDeadOn(kDeviceA, 160);
    Views moved;
    moved.depthRegion = GuideMatch::Region { 0, 0, 960, 540 };
    const auto p = ProbeOn(kDeviceA, 170, moved);
    Execute(p.slot, MakeGrid(3.0));
    Retire(170);
    const auto before = g_lines.size();
    ConsumeMotionProbe(true);
    Check(Said("changed shape", before) == 1 && !g_vk.motionProbe.readbacks.Detector().Dead(),
          "another shape is judged afresh");
}

// ForgetMotionProbe with pins alive: no Vulkan call, the metadata gone, a pin retiring afterwards harmless, nothing
// read.
void ForgetWithLivePins()
{
    FormDeadOn(kDeviceA, 180);
    const auto p = ProbeOn(kDeviceA, 190);
    Execute(p.slot, MakeGrid(9.0, 3));
    const auto destroyed = g_fake.destroyed;
    ForgetMotionProbe();
    Check(g_fake.destroyed == destroyed && DeferredRelease::pending.empty(), "forgetting calls nothing");
    Check(MotionProbeReadyMask() == 0 && !g_vk.motionProbe.pass.Ready(), "every handle forgotten");
    Check(!g_vk.motionProbe.readbacks.Detector().Dead(), "the verdict goes with the device");
    Retire(190);
    const auto fed = g_vk.motionProbe.readbacks.Fed();
    ConsumeMotionProbe(true);
    Check(g_vk.motionProbe.readbacks.Fed() == fed, "a pin retired after forgetting is never read");
}

// DestroyMotionProbe on a drained, unreferenced route: buffers unmapped and freed, grids and the pass through
// DeferredRelease, the verdict dropped.
void DestroyOnDrainedDevice()
{
    FormDeadOn(kDeviceA, 200);
    const auto destroyed = g_fake.destroyed;
    DestroyMotionProbe();
    Check(g_fake.destroyed == destroyed + 4 * 3, "four buffers unmapped, destroyed and their memory freed");
    Check(g_fake.retiredImages == 4, "four grids retired through DeferredRelease");
    Check(DeferredRelease::pending.size() == 1, "the pass's release deferred");
    DeferredRelease::Drain();
    Check(g_fake.destroyed == destroyed + 12 + 3, "the pass's pipeline and layouts destroyed on release");
    Check(!g_vk.motionProbe.readbacks.Detector().Dead() && MotionProbeReadyMask() == 0, "verdict and slots gone");
}

// Failures making the pass: lost on the way (abandoned, nothing destroyed), a grid format the device cannot hold, a
// buffer that cannot be made. Each is the device's, refused without retrying every frame, and cleared with the device.
void CreationFailures()
{
    g_fake.pipelineResult = VK_ERROR_DEVICE_LOST;
    const auto destroyed = g_fake.destroyed;
    const auto e = Evaluate(kDeviceA, 0x9999);
    Check(!e.plan.observable && g_fake.destroyed == destroyed, "lost while made: abandoned, nothing destroyed");
    Check(Said("the device is lost") == 1, "said");
    g_fake.pipelineResult = VK_SUCCESS;
    Check(!Evaluate(kDeviceA, 0x9999).plan.observable, "the failure stands for the device");
    ForgetMotionProbe();
    Check(Evaluate(kDeviceA, 0x9999).plan.observable, "cleared with the device's objects");

    Fresh();
    g_fake.gridFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    Check(!Evaluate(kDeviceA, 0x9999).plan.observable && Said("no R32G32B32A32_SFLOAT storage image") == 1,
          "a device without the grid's format refuses");

    Fresh();
    g_fake.bufferResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    Check(!Evaluate(kDeviceA, 0x9999).plan.observable && Said("could not be allocated") == 1,
          "a readback that cannot be made refuses");
}

// Code review P2: probes recorded before the verdict lost its authority complete afterwards and must not form it again.
// Dead from 0/8/16; three recordings at 24/32/40 capture the old dead field and stay held; a refused evaluate drops
// the verdict; then they retire. Without invalidation they would be baseline, comparison, comparison: Dead again, on no
// probe recorded after the boundary.
void PendingProbesDoNotOutliveTheVerdict()
{
    FormDeadOn(kDeviceA, 300);
    std::vector<int> slots;
    for (uint64_t buffer = 310; buffer < 313; ++buffer)
    {
        const auto p = ProbeOn(kDeviceA, buffer);
        Check(p.slot >= 0, "three recordings hold three slots");
        Execute(p.slot, MakeGrid(3.0 + static_cast<double>(buffer - 310)));
        slots.push_back(p.slot);
    }
    Views refused;
    refused.motion.Resource.ImageViewInfo.Format = VK_FORMAT_R16G16_UINT;
    Check(!Evaluate(kDeviceA, 0x9999, refused).policyReset, "the refused evaluate drops no history");
    Check(!g_vk.motionProbe.readbacks.Detector().Dead(), "and drops the verdict");
    for (uint64_t buffer = 310; buffer < 313; ++buffer)
        Retire(buffer);
    const auto fed = g_vk.motionProbe.readbacks.Fed();
    const auto before = g_lines.size();
    const auto next = Evaluate(kDeviceA, 0x9999);
    Check(next.plan.observable && !next.policyReset, "probes from before the boundary give no policy reset");
    Check(!g_vk.motionProbe.readbacks.Detector().Dead() && Said("read all zero", before) == 0 &&
              g_vk.motionProbe.readbacks.Fed() == fed,
          "probes recorded before the verdict was dropped are freed unread");
    Check(MotionProbeReadyMask() == 0xF && ProbeOn(kDeviceA, 320).slot == slots[0],
          "their slots stayed occupied until the retirement, and are free after it");
    Retire(320);

    // Authority comes back only from probes reserved after the boundary.
    const auto events = FormDeadOn(kDeviceA, 330);
    Check(events.back() == Event::Dead && Evaluate(kDeviceA, 0x9999).policyReset,
          "new probes form the verdict again, and it acts");
}

// Code review P2: a failed vkCreate* leaves its output undefined (here: a poisoned non-null value). The cleanup must
// destroy only what was created, never the failed output.
void LayoutFailureCleansOnlyWhatWasMade()
{
    const VkResult failures[] = { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY };
    for (int which = 0; which < 3; ++which)
        for (const VkResult failure : failures)
        {
            Fresh();
            g_fake.setLayoutResult = which == 0 ? failure : VK_SUCCESS;
            g_fake.layoutResult = which == 1 ? failure : VK_SUCCESS;
            g_fake.pipelineResult = which == 2 ? failure : VK_SUCCESS;
            const auto e = Evaluate(kDeviceA, 0x9999);
            Check(!e.plan.observable && !g_vk.motionProbe.pass.Ready(), "a failed create refuses the probe");
            for (const uint64_t handle : g_fake.unmade)
                Check(handle < Fake::kPoison &&
                          std::find(g_fake.made.begin(), g_fake.made.end(), handle) != g_fake.made.end(),
                      "only handles that were created are destroyed: never a failed create's output");
            auto made = g_fake.made, unmade = g_fake.unmade;
            std::sort(made.begin(), made.end());
            std::sort(unmade.begin(), unmade.end());
            Check(made == unmade, "everything created before the failure is destroyed, once");
        }
}

// With ZeroGuideReset off the route calls none of the probe but its three ends; on a route that never had the key on,
// those make no Vulkan call, allocate nothing and say nothing.
void DefaultOffEnds()
{
    const auto lines = g_lines.size();
    const auto created = g_fake.created, destroyed = g_fake.destroyed;
    g_countAllocations = true;
    g_allocations = 0;
    ForgetMotionProbe();
    AbandonMotionProbeReadbacks();
    DestroyMotionProbe();
    g_countAllocations = false;
    Check(g_allocations == 0, "the ends allocate nothing on an untouched route");
    Check(g_fake.created == created && g_fake.destroyed == destroyed && DeferredRelease::pending.empty(),
          "the ends call nothing on an untouched route");
    Check(g_lines.size() == lines, "and say nothing");
}
} // namespace DlssNr

int main()
{
    using namespace DlssNr;
    const std::pair<const char*, void (*)()> cases[] = {
        { "pure-rules", PureRules },
        { "default-off-ends-inert", DefaultOffEnds },
        { "recorded-commands", RecordedCommands },
        { "baseline-two-pans-dead-then-alive", BaselineTwoPansThenAlive },
        { "sentinel-over-old-valid-grid", SentinelOverOldValidGrid },
        { "out-of-order-retirement", OutOfOrderRetirement },
        { "saturation-resubmission-staleness", SaturationAndStaleness },
        { "dead-on-a-stable-failure-on-b-no-policy-reset", DeadOnAThenStableFailureOnB },
        { "refusal-drops-the-verdict", RefusalDropsTheVerdict },
        { "recording-refusals", RecordingRefusals },
        { "slots-are-exclusive", SlotsAreExclusive },
        { "shape-at-reservation", ShapeAtReservation },
        { "forget-with-live-pins", ForgetWithLivePins },
        { "destroy-on-drained-device", DestroyOnDrainedDevice },
        { "creation-failures", CreationFailures },
        { "pending-probes-do-not-outlive-the-verdict", PendingProbesDoNotOutliveTheVerdict },
        { "layout-failure-cleans-only-what-was-made", LayoutFailureCleansOnlyWhatWasMade },
    };
    int failures = 0;
    for (const auto& [name, run] : cases)
    {
        Fresh();
        try
        {
            run();
            std::cout << "PASS: " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cout << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    Fresh();
    return failures == 0 ? 0 : 1;
}
