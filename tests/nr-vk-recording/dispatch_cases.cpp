#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>
#include "DlssNr_Vk.h"

template <class T> T H(uint64_t value) { return (T) (uintptr_t) value; }
template <class T> uint64_t U(T value) { return (uint64_t) (uintptr_t) value; }
void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
constexpr size_t kTestSlots = 18;
alignas(256) std::array<std::byte, sizeof(DlssNrConstants) * kTestSlots> g_memory;
struct ImageBinding
{
    uint64_t image = 0, sampler = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool operator==(const ImageBinding&) const = default;
};
struct Descriptor
{
    VkDeviceSize offset = 0;
    std::array<ImageBinding, 8> images {};
    bool operator==(const Descriptor&) const = default;
};
struct Recorded
{
    uint64_t set;
    Descriptor descriptor;
    std::array<std::byte, sizeof(DlssNrConstants)> constants;
};
std::vector<Recorded> g_recorded;
std::map<uint64_t, Descriptor> g_descriptors;
std::vector<std::tuple<uint64_t, uint64_t, VkImageLayout, VkImageLayout>> g_layouts;
uint64_t g_bound = 0;
unsigned g_updates = 0, g_dispatches = 0, g_imageCreates = 0;
void TestLayout(VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after)
{
    g_layouts.emplace_back(U(command), U(image), before, after);
}
extern "C"
{
    VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*,
                                                 VkImage* image)
    {
        ++g_imageCreates;
        *image = H<VkImage>(700);
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements* req)
    {
        *req = {};
        req->size = 256;
        req->alignment = 256;
        req->memoryTypeBits = 1;
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*,
                                                    VkDeviceMemory* memory)
    {
        *memory = H<VkDeviceMemory>(701);
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize)
    {
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice, const VkImageViewCreateInfo*,
                                                     const VkAllocationCallbacks*, VkImageView* view)
    {
        *view = H<VkImageView>(702);
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice, uint32_t count, const VkWriteDescriptorSet* writes,
                                                      uint32_t, const VkCopyDescriptorSet*)
    {
        ++g_updates;
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto& write = writes[index];
            Check(write.dstBinding < 8, "descriptor binding out of range");
            auto& descriptor = g_descriptors[U(write.dstSet)];
            if (write.pBufferInfo)
                descriptor.offset = write.pBufferInfo->offset;
            if (write.pImageInfo)
                descriptor.images[write.dstBinding] = { U(write.pImageInfo->imageView), U(write.pImageInfo->sampler),
                                                        write.pImageInfo->imageLayout };
        }
    }
    VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
    VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t,
                                                       uint32_t count, const VkDescriptorSet* sets, uint32_t,
                                                       const uint32_t*)
    {
        Check(count == 1, "one descriptor set expected");
        g_bound = U(sets[0]);
    }
    VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer, uint32_t x, uint32_t y, uint32_t z)
    {
        Check(x == 1 && y == 1 && z == 1, "production dispatch group rounding changed");
        const auto& descriptor = g_descriptors.at(g_bound);
        Check(descriptor.offset + sizeof(DlssNrConstants) <= g_memory.size(),
              "uniform offset outside mapped allocation");
        Recorded recorded { g_bound, descriptor, {} };
        std::memcpy(recorded.constants.data(), g_memory.data() + descriptor.offset, sizeof(DlssNrConstants));
        g_recorded.push_back(recorded);
        ++g_dispatches;
    }
    VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                                                    VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t,
                                                    const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*)
    {
    }
}

DlssNr_Vk::DlssNr_Vk(std::string name, VkDevice device, VkPhysicalDevice physical)
    : Shader_Vk(std::move(name), device, physical)
{
    _slotStride = sizeof(DlssNrConstants);
    _mappedConstantBuffer = g_memory.data();
    _constantBuffer = H<VkBuffer>(1);
    _textureSampler = H<VkSampler>(2);
    _pipeline = H<VkPipeline>(3);
    _pipelineLayout = H<VkPipelineLayout>(4);
    for (uint32_t slot = 0; slot < kTestSlots; ++slot)
        _descriptorSets.push_back(H<VkDescriptorSet>(100 + slot));
    _init = true;
}
DlssNr_Vk::~DlssNr_Vk() = default;

// PRODUCTION_FUNCTIONS

void ResetHarness()
{
    g_memory.fill(std::byte {});
    g_descriptors.clear();
    g_layouts.clear();
    g_recorded.clear();
    g_bound = 0;
    g_updates = 0;
    g_dispatches = 0;
    g_imageCreates = 0;
#if TEST_HAS_RESERVE
    DlssNr::VkRecording::Recordings().ForgetDevice(1);
#endif
}
void Register(VkCommandBuffer command)
{
#if TEST_HAS_RESERVE
    auto& recordings = DlssNr::VkRecording::Recordings();
    recordings.Allocate(1, 5, { U(command) });
    recordings.Begin(U(command));
#else
    (void) command;
#endif
}
bool DispatchOne(DlssNr_Vk& pass, VkCommandBuffer command, unsigned value)
{
    DlssNrConstants constants {};
    constants.Width = value;
    constants.Height = 1;
#if TEST_HAS_RESERVE
    auto batch = pass.Reserve(command, 1);
    if (!batch)
        return false;
    return pass.Dispatch(command, *batch, constants, 8, 8, H<VkImageView>(1000 + value), VK_NULL_HANDLE, VK_NULL_HANDLE,
                         VK_NULL_HANDLE, H<VkImageView>(2000 + value), VK_NULL_HANDLE);
#else
    return pass.Dispatch(command, constants, 8, 8, H<VkImageView>(1000 + value), VK_NULL_HANDLE, VK_NULL_HANDLE,
                         VK_NULL_HANDLE, H<VkImageView>(2000 + value), VK_NULL_HANDLE);
#endif
}
void Exhaustion(bool checkMutation)
{
    ResetHarness();
    DlssNr_Vk pass("integration", H<VkDevice>(1), H<VkPhysicalDevice>(2));
    auto command = H<VkCommandBuffer>(11);
    Register(command);
    for (unsigned value = 1; value <= 18; ++value)
        Check(DispatchOne(pass, command, value), "first eighteen live dispatches must fit");
    Check(g_descriptors.size() == 18, "first eighteen dispatches must use distinct descriptor sets");
    for (const auto& recorded : g_recorded)
    {
        Check(g_descriptors.at(recorded.set) == recorded.descriptor,
              "first eighteen dispatches overwrote an earlier recorded descriptor");
        Check(std::memcmp(recorded.constants.data(), g_memory.data() + recorded.descriptor.offset,
                          sizeof(DlssNrConstants)) == 0,
              "first eighteen dispatches overwrote earlier recorded constants");
    }
    auto memory = g_memory;
    auto descriptors = g_descriptors;
    const auto updates = g_updates, dispatches = g_dispatches;
    const auto layouts = g_layouts.size();
    const bool accepted = DispatchOne(pass, command, 19);
    if (!checkMutation)
        Check(!accepted, "nineteenth live dispatch must be refused, not wrap");
    else
    {
        Check(g_memory == memory, "nineteenth admission overwrote constants referenced by an earlier recording");
        Check(g_descriptors == descriptors, "nineteenth admission rewrote an earlier descriptor set");
        Check(g_updates == updates && g_dispatches == dispatches && g_layouts.size() == layouts,
              "refused admission must not record commands or descriptor writes");
    }
#if TEST_HAS_RESERVE
    DlssNr::VkRecording::Recordings().ForgetDevice(1);
#endif
}
#if TEST_HAS_RESERVE
static_assert(!std::is_copy_constructible_v<DlssNr_Vk::Batch> && !std::is_copy_assignable_v<DlssNr_Vk::Batch>,
              "Batch must not duplicate a mutable slot cursor");
void StaleBatch()
{
    ResetHarness();
    DlssNr_Vk pass("stale-batch", H<VkDevice>(1), H<VkPhysicalDevice>(2));
    auto command = H<VkCommandBuffer>(11);
    Register(command);
    auto batch = pass.Reserve(command, 1);
    Check(batch.has_value(), "initial recording must admit its batch");
    auto& recordings = DlssNr::VkRecording::Recordings();
    recordings.Reset(U(command));
    recordings.Begin(U(command));
    auto memory = g_memory;
    auto descriptors = g_descriptors;
    const auto updates = g_updates, dispatches = g_dispatches, imageCreates = g_imageCreates;
    const auto layouts = g_layouts.size();
    DlssNrConstants constants {};
    constants.Width = 777;
    Check(!pass.Dispatch(command, *batch, constants, 8, 8, H<VkImageView>(1001), VK_NULL_HANDLE, VK_NULL_HANDLE,
                         VK_NULL_HANDLE, H<VkImageView>(2001), VK_NULL_HANDLE),
          "batch reserved before Reset/Begin must not record into a new epoch with the same handle");
    Check(g_memory == memory && g_descriptors == descriptors && g_updates == updates && g_dispatches == dispatches &&
              g_imageCreates == imageCreates && g_layouts.size() == layouts,
          "stale batch rejection must precede constants, descriptor and dummy-image effects");
    recordings.ForgetDevice(1);
}
#endif

void DummyDiscard()
{
    ResetHarness();
    DlssNr_Vk pass("dummy-discard", H<VkDevice>(1), H<VkPhysicalDevice>(2));
    Check(pass.Prepare(H<VkCommandBuffer>(11)), "first dummy preparation failed");
    Check(g_imageCreates == 1 && g_layouts.size() == 1, "first preparation must allocate and record a transition");
    const auto image = std::get<1>(g_layouts.front());
    // No submission occurred: the first recorded transition is discarded.
    Check(pass.Prepare(H<VkCommandBuffer>(12)), "preparation after discarded command buffer failed");
    Check(g_imageCreates == 1, "discarding a recording must not duplicate the placeholder allocation");
    Check(g_layouts.size() == 2,
          "new recording must transition the placeholder after previous recording was discarded");
    Check(std::get<0>(g_layouts[1]) == 12 && std::get<1>(g_layouts[1]) == image &&
              std::get<2>(g_layouts[1]) == VK_IMAGE_LAYOUT_UNDEFINED &&
              std::get<3>(g_layouts[1]) == VK_IMAGE_LAYOUT_GENERAL,
          "second transition must use the second command buffer");
}
int main()
{
    int failed = 0;
    const std::pair<const char*, void (*)()> cases[] = {
        { "v06-live-slot-exhaustion", [] { Exhaustion(false); } },
        { "v06-live-constant-descriptor-immutability", [] { Exhaustion(true); } },
        { "dummy-transition-after-recording-discard", DummyDiscard },
#if TEST_HAS_RESERVE
        { "v06-stale-batch-after-reset-begin", StaleBatch },
#endif
    };
    for (auto [name, run] : cases)
        try
        {
            run();
            std::cout << "PASS: " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failed;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
#if TEST_HAS_RESERVE
    DlssNr::VkRecording::Recordings().ForgetDevice(1);
#endif
    return failed ? 1 : 0;
}
