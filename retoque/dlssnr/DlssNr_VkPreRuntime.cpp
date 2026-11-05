#include "pch.h"

#include "DlssNr_VkPreRuntime.h"
#include <Config.h>
#include <hooks/Vulkan_Hooks.h>
#include <proxies/KernelBase_Proxy.h>
#include <detours/detours.h>
#include <atomic>
#include <array>
#include <mutex>
#include <memory>
#include <type_traits>
#include <unordered_set>

namespace DlssNr::VkPre::Runtime
{
namespace
{
std::mutex installMutex;
struct Storage
{
    std::unordered_set<uint64_t> devices;
    std::unordered_set<PFN_vkVoidFunction> attachedAddresses;
    std::unordered_map<uint64_t, HookStatus> coverage;
    Observations facts;
};
// MSVC's empty unordered containers may allocate. No container is constructed on the default/off path.
std::unique_ptr<Storage> storageOwner;
std::atomic<Storage*> storage { nullptr };
std::atomic<bool> installed { false }, blind { false };
template <class T> uint64_t H(T value) { return (uint64_t) value; }
bool Wanted()
{
    const auto& cfg = *Config::Instance();
    return cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrStage.value_or_default() == 1;
}
bool Active()
{
    if (!Wanted())
    {
        blind.store(true, std::memory_order_relaxed);
        return false;
    }
    return !blind.load(std::memory_order_relaxed);
}
template <class Fn> void Observe(Fn&& fn) noexcept
{
    auto* data = storage.load(std::memory_order_acquire);
    if (!data || !Active())
        return;
    try
    {
        fn(data->facts);
    }
    catch (...)
    {
        blind.store(true, std::memory_order_relaxed);
    }
}
Range R(VkImageSubresourceRange r)
{
    return { r.aspectMask, r.baseMipLevel, r.levelCount, r.baseArrayLayer, r.layerCount };
}

// Separate trampoline per target address. Export -> ICD nesting of the same API is observed only once.
// Each transaction is independent: a failed optional hook never strands an uninitialized trampoline.
template <class Event, class Pfn> struct Hooks;
template <class Event, class Result, class... Args> struct Hooks<Event, Result(VKAPI_PTR*)(Args...)>
{
    using Pfn = Result(VKAPI_PTR*)(Args...);
    inline static std::array<Pfn, 4> next {};
    inline static std::array<PFN_vkVoidFunction, 4> target {};
    inline static thread_local unsigned depth = 0;
    template <size_t Slot> static Result VKAPI_CALL Call(Args... args)
    {
        struct Nest
        {
            Nest() { ++depth; }
            ~Nest() { --depth; }
        } nest;
        if constexpr (std::is_void_v<Result>)
        {
            next[Slot](args...);
            if (depth == 1)
                Event::After(args...);
        }
        else
        {
            if constexpr (requires { Event::Before(args...); })
            {
                const auto before = depth == 1 ? Event::Before(args...) : std::optional<Ticket> {};
                const Result result = next[Slot](args...);
                if (depth == 1)
                    Event::After(before, result, args...);
                return result;
            }
            else
            {
                const Result result = next[Slot](args...);
                if (depth == 1)
                    Event::After(result, args...);
                return result;
            }
        }
    }
    static bool Attach(PFN_vkVoidFunction address)
    {
        if (!address)
            return false;
        for (auto existing : target)
            if (existing == address)
                return true;
        if (storageOwner->attachedAddresses.count(address))
            return false;
        const std::array<Pfn, 4> hooks { Call<0>, Call<1>, Call<2>, Call<3> };
        for (size_t i = 0; i < target.size(); ++i)
        {
            if (target[i])
                continue;
            next[i] = reinterpret_cast<Pfn>(address);
            if (DetourTransactionBegin() != NO_ERROR)
            {
                next[i] = nullptr;
                return false;
            }
            if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
                DetourAttach(reinterpret_cast<PVOID*>(&next[i]), reinterpret_cast<PVOID>(hooks[i])) != NO_ERROR)
            {
                DetourTransactionAbort();
                next[i] = nullptr;
                return false;
            }
            if (DetourTransactionCommit() != NO_ERROR)
            {
                next[i] = nullptr;
                return false;
            }
            target[i] = address;
            storageOwner->attachedAddresses.insert(address);
            return true;
        }
        return false;
    }
};

struct CreateImage
{
    static void After(VkResult result, VkDevice d, const VkImageCreateInfo* i, const VkAllocationCallbacks*,
                      VkImage* out) noexcept
    {
        if (result != VK_SUCCESS || !i || !out)
            return;
        Observe(
            [&](auto& r)
            {
                ImageFact f;
                f.device = H(d);
                f.width = i->extent.width;
                f.height = i->extent.height;
                f.depth = i->extent.depth;
                f.levels = i->mipLevels;
                f.layers = i->arrayLayers;
                f.format = i->format;
                f.usage = i->usage;
                f.samples = i->samples;
                f.type = i->imageType;
                f.simple = !i->pNext && !i->flags;
                f.concurrent = i->sharingMode == VK_SHARING_MODE_CONCURRENT;
                if (i->queueFamilyIndexCount && i->pQueueFamilyIndices)
                    f.families.assign(i->pQueueFamilyIndices, i->pQueueFamilyIndices + i->queueFamilyIndexCount);
                r.Image(H(*out), std::move(f));
            });
    }
};

struct DestroyImage
{
    static void After(VkDevice d, VkImage image, const VkAllocationCallbacks*) noexcept
    {
        Observe([&](auto& r) { r.DestroyImage(H(d), H(image)); });
    }
};

struct CreateImageView
{
    static void After(VkResult result, VkDevice d, const VkImageViewCreateInfo* i, const VkAllocationCallbacks*,
                      VkImageView* out) noexcept
    {
        if (result != VK_SUCCESS || !i || !out)
            return;
        Observe(
            [&](auto& r)
            {
                ViewFact f { H(d), H(i->image), uint32_t(i->format), uint32_t(i->viewType), R(i->subresourceRange) };
                f.simple = !i->pNext && !i->flags && i->components.r == VK_COMPONENT_SWIZZLE_IDENTITY &&
                           i->components.g == VK_COMPONENT_SWIZZLE_IDENTITY &&
                           i->components.b == VK_COMPONENT_SWIZZLE_IDENTITY &&
                           i->components.a == VK_COMPONENT_SWIZZLE_IDENTITY;
                r.View(H(*out), f);
            });
    }
};

struct DestroyImageView
{
    static void After(VkDevice d, VkImageView view, const VkAllocationCallbacks*) noexcept
    {
        Observe([&](auto& r) { r.DestroyView(H(d), H(view)); });
    }
};

struct CreateCommandPool
{
    static void After(VkResult result, VkDevice d, const VkCommandPoolCreateInfo* i, const VkAllocationCallbacks*,
                      VkCommandPool* out) noexcept
    {
        if (result == VK_SUCCESS && i && out)
            Observe([&](auto& r) { r.Pool(H(d), H(*out), i->queueFamilyIndex); });
    }
};

struct DestroyCommandPool
{
    static void After(VkDevice d, VkCommandPool pool, const VkAllocationCallbacks*) noexcept
    {
        Observe([&](auto& r) { r.DestroyPool(H(d), H(pool)); });
    }
};

struct ResetCommandPool
{
    static void After(VkResult result, VkDevice d, VkCommandPool pool, VkCommandPoolResetFlags) noexcept
    {
        if (result == VK_SUCCESS)
            Observe([&](auto& r) { r.ResetPool(H(d), H(pool), true); });
    }
};

struct AllocateCommandBuffers
{
    static void After(VkResult result, VkDevice d, const VkCommandBufferAllocateInfo* i, VkCommandBuffer* out) noexcept
    {
        if (result == VK_SUCCESS && i && out)
            Observe(
                [&](auto& r)
                {
                    for (uint32_t n = 0; n < i->commandBufferCount; ++n)
                        r.Allocate(H(d), H(i->commandPool), H(out[n]), i->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);
                });
    }
};

struct FreeCommandBuffers
{
    static void After(VkDevice d, VkCommandPool, uint32_t count, const VkCommandBuffer* buffers) noexcept
    {
        if (buffers)
            Observe(
                [&](auto& r)
                {
                    for (uint32_t n = 0; n < count; ++n)
                        r.Free(H(d), H(buffers[n]));
                });
    }
};

struct BeginCommandBuffer
{
    static std::optional<Ticket> Before(VkCommandBuffer c, const VkCommandBufferBeginInfo*) noexcept
    {
        std::optional<Ticket> prior;
        Observe(
            [&](auto& r)
            {
                if (auto device = r.DeviceOfCommand(H(c)))
                    prior = VkRecording::Recordings().Current(*device, H(c));
            });
        return prior;
    }
    static void After(std::optional<Ticket> prior, VkResult result, VkCommandBuffer c,
                      const VkCommandBufferBeginInfo* i) noexcept
    {
        if (result == VK_SUCCESS && i)
            Observe(
                [&](auto& r)
                {
                    std::optional<Ticket> current;
                    if (auto device = r.DeviceOfCommand(H(c)))
                        current = VkRecording::Recordings().Current(*device, H(c));
                    r.Begin(H(c), true, (i->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != 0,
                            (i->flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) != 0, prior, current);
                });
    }
};

struct EndCommandBuffer
{
    static void After(VkResult result, VkCommandBuffer c) noexcept
    {
        if (result == VK_SUCCESS)
            Observe([&](auto& r) { r.End(H(c), true); });
    }
};

struct ResetCommandBuffer
{
    static void After(VkResult result, VkCommandBuffer c, VkCommandBufferResetFlags) noexcept
    {
        if (result == VK_SUCCESS)
            Observe([&](auto& r) { r.ResetBuffer(H(c), true); });
    }
};

struct DestroyDevice
{
    static void After(VkDevice d, const VkAllocationCallbacks*) noexcept
    {
        // Erasure is needed even after the observation window closes; no GPU object is owned here.
        try
        {
            std::lock_guard lock(installMutex);
            if (auto* data = storage.load(std::memory_order_acquire))
            {
                data->facts.DestroyDevice(H(d));
                data->devices.erase(H(d));
                data->coverage.erase(H(d));
            }
        }
        catch (...)
        {
            blind.store(true, std::memory_order_relaxed);
        }
    }
};

struct CmdPipelineBarrier
{
    static void After(VkCommandBuffer c, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                      const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t count,
                      const VkImageMemoryBarrier* barriers) noexcept
    {
        if (barriers)
            Observe(
                [&](auto& r)
                {
                    for (uint32_t n = 0; n < count; ++n)
                    {
                        const auto& b = barriers[n];
                        r.Barrier(H(c), H(b.image), R(b.subresourceRange), b.oldLayout, b.newLayout,
                                  b.srcQueueFamilyIndex, b.dstQueueFamilyIndex);
                    }
                });
    }
};

struct CmdPipelineBarrier2
{
    static void After(VkCommandBuffer c, const VkDependencyInfo* i) noexcept
    {
        if (i && i->pImageMemoryBarriers)
            Observe(
                [&](auto& r)
                {
                    for (uint32_t n = 0; n < i->imageMemoryBarrierCount; ++n)
                    {
                        const auto& b = i->pImageMemoryBarriers[n];
                        r.Barrier(H(c), H(b.image), R(b.subresourceRange), b.oldLayout, b.newLayout,
                                  b.srcQueueFamilyIndex, b.dstQueueFamilyIndex);
                    }
                });
    }
};

struct CmdBeginRenderPass
{
    static void After(VkCommandBuffer c, const VkRenderPassBeginInfo*, VkSubpassContents) noexcept
    {
        Observe([&](auto& r) { r.Rendering(H(c), true, true); });
    }
};

struct CmdEndRenderPass
{
    static void After(VkCommandBuffer c) noexcept
    {
        Observe([&](auto& r) { r.Rendering(H(c), false, true); });
    }
};

struct CmdBeginRenderPass2
{
    static void After(VkCommandBuffer c, const VkRenderPassBeginInfo*, const VkSubpassBeginInfo*) noexcept
    {
        Observe([&](auto& r) { r.Rendering(H(c), true, true); });
    }
};

struct CmdEndRenderPass2
{
    static void After(VkCommandBuffer c, const VkSubpassEndInfo*) noexcept
    {
        Observe([&](auto& r) { r.Rendering(H(c), false, true); });
    }
};

struct CmdBeginRendering
{
    static void After(VkCommandBuffer c, const VkRenderingInfo* i) noexcept
    {
        Observe(
            [&](auto& r)
            {
                r.Rendering(H(c), true, false);
                if (!i || i->pNext || i->flags)
                    r.ExecuteSecondary(H(c));
            });
    }
};

struct CmdEndRendering
{
    static void After(VkCommandBuffer c) noexcept
    {
        Observe([&](auto& r) { r.Rendering(H(c), false, false); });
    }
};

struct CmdBindPipeline
{
    static void After(VkCommandBuffer c, VkPipelineBindPoint point, VkPipeline pipeline) noexcept
    {
        Observe([&](auto& r) { r.Pipeline(H(c), point, H(pipeline)); });
    }
};

struct CmdBindDescriptorSets
{
    static void After(VkCommandBuffer c, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
                      uint32_t count, const VkDescriptorSet* sets, uint32_t offsetCount,
                      const uint32_t* offsets) noexcept
    {
        Observe(
            [&](auto& r)
            {
                if (count > 32 || (count && !sets))
                {
                    r.Unsupported(H(c));
                    return;
                }
                std::array<uint64_t, 32> values {};
                for (uint32_t n = 0; n < count; ++n)
                    values[n] = H(sets[n]);
                r.Descriptors(H(c), point, H(layout), first, count, values.data(), offsetCount, offsets);
            });
    }
};

struct CmdPushConstants
{
    static void After(VkCommandBuffer c, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                      uint32_t size, const void* data) noexcept
    {
        Observe([&](auto& r) { r.Push(H(c), H(layout), stages, offset, size, data); });
    }
};

struct CmdExecuteCommands
{
    static void After(VkCommandBuffer c, uint32_t, const VkCommandBuffer*) noexcept
    {
        Observe([&](auto& r) { r.ExecuteSecondary(H(c)); });
    }
};

struct CmdPushDescriptorSet
{
    static void After(VkCommandBuffer c, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                      const VkWriteDescriptorSet*) noexcept
    {
        Observe([&](auto& r) { r.Unsupported(H(c)); });
    }
};

struct CmdPushDescriptorSetWithTemplate
{
    static void After(VkCommandBuffer c, VkDescriptorUpdateTemplate, VkPipelineLayout, uint32_t, const void*) noexcept
    {
        Observe([&](auto& r) { r.Unsupported(H(c)); });
    }
};

struct CmdBindDescriptorSets2
{
    static void After(VkCommandBuffer c, const VkBindDescriptorSetsInfo*) noexcept
    {
        Observe([&](auto& r) { r.Unsupported(H(c)); });
    }
};

struct CmdPushConstants2
{
    static void After(VkCommandBuffer c, const VkPushConstantsInfo*) noexcept
    {
        Observe([&](auto& r) { r.Unsupported(H(c)); });
    }
};

struct CmdBindShadersEXT
{
    static void After(VkCommandBuffer c, uint32_t, const VkShaderStageFlagBits*, const VkShaderEXT*) noexcept
    {
        Observe([&](auto& r) { r.Unsupported(H(c)); });
    }
};

struct CmdBindDescriptorBuffersEXT
{
    static void After(VkCommandBuffer c, uint32_t, const VkDescriptorBufferBindingInfoEXT*) noexcept
    {
        Observe([&](auto& r) { r.Unsupported(H(c)); });
    }
};

struct CmdSetDescriptorBufferOffsetsEXT
{
    static void After(VkCommandBuffer c, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const uint32_t*,
                      const VkDeviceSize*) noexcept
    {
        Observe([&](auto& r) { r.Unsupported(H(c)); });
    }
};

struct CmdWaitEvents
{
    static void After(VkCommandBuffer c, uint32_t, const VkEvent*, VkPipelineStageFlags, VkPipelineStageFlags, uint32_t,
                      const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                      const VkImageMemoryBarrier*) noexcept
    {
        Observe([&](auto& r) { r.ExecuteSecondary(H(c)); });
    }
};

struct CmdWaitEvents2
{
    static void After(VkCommandBuffer c, uint32_t, const VkEvent*, const VkDependencyInfo*) noexcept
    {
        Observe([&](auto& r) { r.ExecuteSecondary(H(c)); });
    }
};

struct CmdPushDescriptorSet2
{
    static void After(VkCommandBuffer c, const VkPushDescriptorSetInfo*) noexcept
    {
        Observe([&](auto& r) { r.Unsupported(H(c)); });
    }
};

struct CmdPushDescriptorSetWithTemplate2
{
    static void After(VkCommandBuffer c, const VkPushDescriptorSetWithTemplateInfo*) noexcept
    {
        Observe([&](auto& r) { r.Unsupported(H(c)); });
    }
};

struct CmdBindDescriptorBufferEmbeddedSamplersEXT
{
    static void After(VkCommandBuffer c, VkPipelineBindPoint, VkPipelineLayout, uint32_t) noexcept
    {
        Observe([&](auto& r) { r.Unsupported(H(c)); });
    }
};

struct CmdBindDescriptorBufferEmbeddedSamplers2EXT
{
    static void After(VkCommandBuffer c, const VkBindDescriptorBufferEmbeddedSamplersInfoEXT*) noexcept
    {
        Observe([&](auto& r) { r.Unsupported(H(c)); });
    }
};

struct CmdSetDescriptorBufferOffsets2EXT
{
    static void After(VkCommandBuffer c, const VkSetDescriptorBufferOffsetsInfoEXT*) noexcept
    {
        Observe([&](auto& r) { r.Unsupported(H(c)); });
    }
};

template <class Event, class Pfn>
void InstallApi(VkDevice device, HMODULE module, const char* name, bool required = true, bool equivalentPresent = false)
{
    // Observe both proc-address and import/export targets. A promoted spelling may be absent when its
    // equivalent is available, but every callable address must still attach for a required command.
    auto address = VulkanHooks::GetDeviceProcAddr(device, name);
    auto& status = storageOwner->coverage[H(device)];
    if (address)
    {
        if (Hooks<Event, Pfn>::Attach(address))
            ++status.attached;
        else
        {
            ++status.failed;
            status.requiredFailed += required;
        }
    }
    else
    {
        ++status.missing;
        status.requiredMissing += required && !equivalentPresent;
    }
    if (module)
    {
        auto exported = reinterpret_cast<PFN_vkVoidFunction>(KernelBaseProxy::GetProcAddress_()(module, name));
        if (exported)
        {
            if (Hooks<Event, Pfn>::Attach(exported))
                ++status.attached;
            else
            {
                ++status.failed;
                status.requiredFailed += required;
            }
        }
    }
}
template <class Event, class Pfn>
void InstallPromotedApi(VkDevice device, HMODULE module, const char* core, const char* extension)
{
    // Device-proc availability establishes applicability, not the presence of a loader export stub.
    // Do not make the alias optional wholesale: a distinct callable alias that fails attachment is a gap.
    const bool available =
        VulkanHooks::GetDeviceProcAddr(device, core) || VulkanHooks::GetDeviceProcAddr(device, extension);
    InstallApi<Event, Pfn>(device, module, core, true, available);
    InstallApi<Event, Pfn>(device, module, extension, true, available);
}
} // namespace
std::optional<Ticket> CurrentTicket(VkDevice device, VkCommandBuffer buffer) noexcept
{
    std::optional<Ticket> result;
    Observe(
        [&](auto& r)
        {
            const auto current = VkRecording::Recordings().Current(H(device), H(buffer));
            result = r.Associated(H(device), H(buffer), current);
        });
    return result;
}

Observations* Facts() noexcept
{
    auto* data = storage.load(std::memory_order_acquire);
    return data ? &data->facts : nullptr;
}
bool Uninterrupted() noexcept
{
    return installed.load(std::memory_order_relaxed) && !blind.load(std::memory_order_relaxed);
}
void Install(VkDevice device, const VkDeviceCreateInfo* info, uint32_t apiVersion) noexcept
{
    if (!device || !Wanted())
        return;
    try
    {
        std::lock_guard lock(installMutex);
        if (!storageOwner)
        {
            storageOwner = std::make_unique<Storage>();
            storage.store(storageOwner.get(), std::memory_order_release);
        }
        auto& capability = storageOwner->coverage[H(device)];
        capability.formatless |= apiVersion >= VK_API_VERSION_1_3;
        capability.formatFlags2 |= apiVersion >= VK_API_VERSION_1_3;
        if (info)
        {
            capability.formatless |=
                info->pEnabledFeatures && info->pEnabledFeatures->shaderStorageImageWriteWithoutFormat != VK_FALSE;
            for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext)
                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
                    capability.formatless |= reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)
                                                 ->features.shaderStorageImageWriteWithoutFormat != VK_FALSE;
            for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
                if (info->ppEnabledExtensionNames &&
                    std::strcmp(info->ppEnabledExtensionNames[i], VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME) == 0)
                    capability.formatFlags2 = true;
        }
        if (!storageOwner->devices.insert(H(device)).second)
            return;
        storageOwner->facts.Device(H(device));
        const auto module = GetModuleHandleW(L"vulkan-1.dll");
        InstallApi<CreateImage, PFN_vkCreateImage>(device, module, "vkCreateImage");
        InstallApi<DestroyImage, PFN_vkDestroyImage>(device, module, "vkDestroyImage");
        InstallApi<CreateImageView, PFN_vkCreateImageView>(device, module, "vkCreateImageView");
        InstallApi<DestroyImageView, PFN_vkDestroyImageView>(device, module, "vkDestroyImageView");
        InstallApi<CreateCommandPool, PFN_vkCreateCommandPool>(device, module, "vkCreateCommandPool");
        InstallApi<DestroyCommandPool, PFN_vkDestroyCommandPool>(device, module, "vkDestroyCommandPool");
        InstallApi<ResetCommandPool, PFN_vkResetCommandPool>(device, module, "vkResetCommandPool");
        InstallApi<AllocateCommandBuffers, PFN_vkAllocateCommandBuffers>(device, module, "vkAllocateCommandBuffers");
        InstallApi<FreeCommandBuffers, PFN_vkFreeCommandBuffers>(device, module, "vkFreeCommandBuffers");
        InstallApi<BeginCommandBuffer, PFN_vkBeginCommandBuffer>(device, module, "vkBeginCommandBuffer");
        InstallApi<EndCommandBuffer, PFN_vkEndCommandBuffer>(device, module, "vkEndCommandBuffer");
        InstallApi<ResetCommandBuffer, PFN_vkResetCommandBuffer>(device, module, "vkResetCommandBuffer");
        InstallApi<DestroyDevice, PFN_vkDestroyDevice>(device, module, "vkDestroyDevice");
        InstallApi<CmdPipelineBarrier, PFN_vkCmdPipelineBarrier>(device, module, "vkCmdPipelineBarrier");
        InstallPromotedApi<CmdPipelineBarrier2, PFN_vkCmdPipelineBarrier2>(device, module, "vkCmdPipelineBarrier2",
                                                                           "vkCmdPipelineBarrier2KHR");
        InstallApi<CmdBeginRenderPass, PFN_vkCmdBeginRenderPass>(device, module, "vkCmdBeginRenderPass");
        InstallApi<CmdEndRenderPass, PFN_vkCmdEndRenderPass>(device, module, "vkCmdEndRenderPass");
        InstallPromotedApi<CmdBeginRenderPass2, PFN_vkCmdBeginRenderPass2>(device, module, "vkCmdBeginRenderPass2",
                                                                           "vkCmdBeginRenderPass2KHR");
        InstallPromotedApi<CmdEndRenderPass2, PFN_vkCmdEndRenderPass2>(device, module, "vkCmdEndRenderPass2",
                                                                       "vkCmdEndRenderPass2KHR");
        InstallPromotedApi<CmdBeginRendering, PFN_vkCmdBeginRendering>(device, module, "vkCmdBeginRendering",
                                                                       "vkCmdBeginRenderingKHR");
        InstallPromotedApi<CmdEndRendering, PFN_vkCmdEndRendering>(device, module, "vkCmdEndRendering",
                                                                   "vkCmdEndRenderingKHR");
        InstallApi<CmdBindPipeline, PFN_vkCmdBindPipeline>(device, module, "vkCmdBindPipeline", false);
        InstallApi<CmdBindDescriptorSets, PFN_vkCmdBindDescriptorSets>(device, module, "vkCmdBindDescriptorSets",
                                                                       false);
        InstallApi<CmdPushConstants, PFN_vkCmdPushConstants>(device, module, "vkCmdPushConstants", false);
        InstallApi<CmdExecuteCommands, PFN_vkCmdExecuteCommands>(device, module, "vkCmdExecuteCommands");
        InstallApi<CmdPushDescriptorSet, PFN_vkCmdPushDescriptorSet>(device, module, "vkCmdPushDescriptorSet", false);
        InstallApi<CmdPushDescriptorSet, PFN_vkCmdPushDescriptorSet>(device, module, "vkCmdPushDescriptorSetKHR",
                                                                     false);
        InstallApi<CmdPushDescriptorSetWithTemplate, PFN_vkCmdPushDescriptorSetWithTemplate>(
            device, module, "vkCmdPushDescriptorSetWithTemplate", false);
        InstallApi<CmdPushDescriptorSetWithTemplate, PFN_vkCmdPushDescriptorSetWithTemplate>(
            device, module, "vkCmdPushDescriptorSetWithTemplateKHR", false);
        InstallApi<CmdBindDescriptorSets2, PFN_vkCmdBindDescriptorSets2>(device, module, "vkCmdBindDescriptorSets2",
                                                                         false);
        InstallApi<CmdBindDescriptorSets2, PFN_vkCmdBindDescriptorSets2>(device, module, "vkCmdBindDescriptorSets2KHR",
                                                                         false);
        InstallApi<CmdPushConstants2, PFN_vkCmdPushConstants2>(device, module, "vkCmdPushConstants2", false);
        InstallApi<CmdPushConstants2, PFN_vkCmdPushConstants2>(device, module, "vkCmdPushConstants2KHR", false);
        InstallApi<CmdBindShadersEXT, PFN_vkCmdBindShadersEXT>(device, module, "vkCmdBindShadersEXT", false);
        InstallApi<CmdBindDescriptorBuffersEXT, PFN_vkCmdBindDescriptorBuffersEXT>(
            device, module, "vkCmdBindDescriptorBuffersEXT", false);
        InstallApi<CmdSetDescriptorBufferOffsetsEXT, PFN_vkCmdSetDescriptorBufferOffsetsEXT>(
            device, module, "vkCmdSetDescriptorBufferOffsetsEXT", false);
        InstallApi<CmdWaitEvents, PFN_vkCmdWaitEvents>(device, module, "vkCmdWaitEvents");
        InstallPromotedApi<CmdWaitEvents2, PFN_vkCmdWaitEvents2>(device, module, "vkCmdWaitEvents2",
                                                                 "vkCmdWaitEvents2KHR");
        InstallApi<CmdPushDescriptorSet2, PFN_vkCmdPushDescriptorSet2>(device, module, "vkCmdPushDescriptorSet2",
                                                                       false);
        InstallApi<CmdPushDescriptorSet2, PFN_vkCmdPushDescriptorSet2>(device, module, "vkCmdPushDescriptorSet2KHR",
                                                                       false);
        InstallApi<CmdPushDescriptorSetWithTemplate2, PFN_vkCmdPushDescriptorSetWithTemplate2>(
            device, module, "vkCmdPushDescriptorSetWithTemplate2", false);
        InstallApi<CmdPushDescriptorSetWithTemplate2, PFN_vkCmdPushDescriptorSetWithTemplate2>(
            device, module, "vkCmdPushDescriptorSetWithTemplate2KHR", false);
        InstallApi<CmdBindDescriptorBufferEmbeddedSamplersEXT, PFN_vkCmdBindDescriptorBufferEmbeddedSamplersEXT>(
            device, module, "vkCmdBindDescriptorBufferEmbeddedSamplersEXT", false);
        InstallApi<CmdBindDescriptorBufferEmbeddedSamplers2EXT, PFN_vkCmdBindDescriptorBufferEmbeddedSamplers2EXT>(
            device, module, "vkCmdBindDescriptorBufferEmbeddedSamplers2EXT", false);
        InstallApi<CmdSetDescriptorBufferOffsets2EXT, PFN_vkCmdSetDescriptorBufferOffsets2EXT>(
            device, module, "vkCmdSetDescriptorBufferOffsets2EXT", false);
        auto& status = storageOwner->coverage[H(device)];
        status.complete = status.requiredMissing == 0 && status.requiredFailed == 0;
        installed.store(true, std::memory_order_relaxed);
    }
    catch (...)
    {
        blind.store(true, std::memory_order_relaxed);
    }
}
HookStatus Status(VkDevice device) noexcept
{
    try
    {
        std::lock_guard lock(installMutex);
        if (!storageOwner)
            return {};
        auto it = storageOwner->coverage.find(H(device));
        if (it != storageOwner->coverage.end())
            return it->second;
    }
    catch (...)
    {
    }
    return {};
}
void RememberFeature(VkDevice device, uint64_t handle, Feature kind, bool success, uint64_t source) noexcept
{
    if (!success || !Wanted())
        return;
    Observe([&](auto& r) { r.FeatureCreated(H(device), handle, kind, source); });
}
void ForgetFeature(uint64_t source, uint64_t handle) noexcept
{
    if (!installed.load(std::memory_order_relaxed))
        return;
    try
    {
        if (auto* facts = Facts())
            facts->ForgetFeatureSource(source, handle);
    }
    catch (...)
    {
        blind.store(true, std::memory_order_relaxed);
    }
}
} // namespace DlssNr::VkPre::Runtime
