#include "pch.h"
// Compile the production backend, with Vulkan functions implemented below. No extraction or parallel implementation.
#include "../../retoque/dlssnr/DlssNr_PairCaptureVk.cpp"
#include <cassert>
#include <map>
#include <iostream>
using namespace DlssNr;
namespace fake
{
uintptr_t next = 10;
template <class T> T Handle() { return (T) ++next; }
struct Buf
{
    uint64_t size;
    VkDeviceMemory memory;
};
std::map<VkBuffer, Buf> buffers;
std::map<VkDeviceMemory, std::vector<uint8_t>> memory;
std::map<VkDescriptorSet, VkBuffer> sets;
std::vector<std::function<void()>> commands;
VkDescriptorSet bound {};
std::shared_ptr<PairCapture::CpuJob> job;
unsigned created = 0, destroyed = 0, copies = 0, barriers = 0;
uint64_t padding = 256;
bool failAlloc = false, execute = true;
void Submit()
{
    for (auto& f : commands)
        f();
}
void Reset()
{
    VkRecording::Recordings().Reset(3);
    commands.clear();
}
} // namespace fake
namespace DlssNr
{
std::mutex& PairCaptureVkMutex()
{
    static std::mutex mutex;
    return mutex;
}
namespace PairCapture
{
bool DirectoryFailed() { return false; }
bool EnqueueCpuPair(std::shared_ptr<CpuJob> job)
{
    fake::job = std::move(job);
    return true;
}
} // namespace PairCapture
} // namespace DlssNr
extern "C"
{
    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* p)
    {
        *p = {};
        p->limits.maxStorageBufferRange = 1u << 28;
        p->limits.maxComputeWorkGroupCount[0] = 65535;
        p->limits.maxComputeWorkGroupCount[1] = 65535;
    }
    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice,
                                                                   VkPhysicalDeviceMemoryProperties* p)
    {
        *p = {};
        p->memoryTypeCount = 1;
        p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo* i, const VkAllocationCallbacks*,
                                                  VkBuffer* b)
    {
        *b = fake::Handle<VkBuffer>();
        fake::buffers[*b] = { i->size, {} };
        ++fake::created;
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer b, VkMemoryRequirements* r)
    {
        *r = {};
        r->size = fake::buffers[b].size + fake::padding;
        r->memoryTypeBits = 1;
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo* i,
                                                    const VkAllocationCallbacks*, VkDeviceMemory* m)
    {
        if (fake::failAlloc)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        *m = fake::Handle<VkDeviceMemory>();
        fake::memory[*m].resize(i->allocationSize);
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer b, VkDeviceMemory m, VkDeviceSize)
    {
        fake::buffers[b].memory = m;
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice, VkDeviceMemory m, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags,
                                               void** p)
    {
        *p = fake::memory[m].data();
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory) {}
    VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer b, const VkAllocationCallbacks*)
    {
        fake::buffers.erase(b);
        ++fake::destroyed;
    }
    VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory m, const VkAllocationCallbacks*)
    {
        fake::memory.erase(m);
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo*,
                                                               const VkAllocationCallbacks*, VkDescriptorSetLayout* p)
    {
        *p = fake::Handle<VkDescriptorSetLayout>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo*,
                                                          const VkAllocationCallbacks*, VkPipelineLayout* p)
    {
        *p = fake::Handle<VkPipelineLayout>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo* i,
                                                        const VkAllocationCallbacks*, VkShaderModule* p)
    {
        assert(i->pCode[0] == 0x07230203);
        *p = fake::Handle<VkShaderModule>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t,
                                                            const VkComputePipelineCreateInfo*,
                                                            const VkAllocationCallbacks*, VkPipeline* p)
    {
        *p = fake::Handle<VkPipeline>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo*,
                                                          const VkAllocationCallbacks*, VkDescriptorPool* p)
    {
        *p = fake::Handle<VkDescriptorPool>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo*,
                                                            VkDescriptorSet* p)
    {
        p[0] = fake::Handle<VkDescriptorSet>();
        p[1] = fake::Handle<VkDescriptorSet>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice, uint32_t n, const VkWriteDescriptorSet* w, uint32_t,
                                                      const VkCopyDescriptorSet*)
    {
        for (uint32_t i = 0; i < n; ++i)
            if (w[i].pBufferInfo)
                fake::sets[w[i].dstSet] = w[i].pBufferInfo->buffer;
    }
    VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) {}
    VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) {}
    VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {}
    VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*) {}
    VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout,
                                                            const VkAllocationCallbacks*)
    {
    }
    VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                                                    VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t nb,
                                                    const VkBufferMemoryBarrier* b, uint32_t ni,
                                                    const VkImageMemoryBarrier* i)
    {
        ++fake::barriers;
        for (uint32_t j = 0; j < ni; ++j)
            assert(i[j].oldLayout == VK_IMAGE_LAYOUT_GENERAL && i[j].newLayout == VK_IMAGE_LAYOUT_GENERAL &&
                   i[j].srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
        for (uint32_t j = 0; j < nb; ++j)
            assert(b[j].srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    }
    VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
    VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t,
                                                       uint32_t, const VkDescriptorSet* p, uint32_t, const uint32_t*)
    {
        fake::bound = *p;
    }
    VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t,
                                                  uint32_t, const void*)
    {
    }
    VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t)
    {
        const auto b = fake::sets[fake::bound];
        const uint8_t pixel = uint8_t(++fake::copies);
        fake::commands.push_back(
            [b, pixel]
            {
                auto& mem = fake::memory[fake::buffers[b].memory];
                std::fill(mem.begin(), mem.begin() + 64, pixel);
            });
    }
    VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer, VkBuffer b, VkDeviceSize offset, VkDeviceSize size,
                                                 const void* data)
    {
        assert(size == 8);
        const uint64_t cookie = *static_cast<const uint64_t*>(data);
        fake::commands.push_back([b, offset, cookie]
                                 { std::memcpy(fake::memory[fake::buffers[b].memory].data() + offset, &cookie, 8); });
    }
}
PairCapture::VkFrame Frame()
{
    PairCapture::VkFrame f;
    f.device = (VkDevice) 1;
    f.physical = (VkPhysicalDevice) 2;
    f.cmd = (VkCommandBuffer) 3;
    f.image = (VkImage) 4;
    f.view = (VkImageView) 5;
    f.range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    f.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    f.facts.width = 2;
    f.facts.height = 2;
    f.facts.linearHdr = true;
    f.recording = *VkRecording::Recordings().Current(1, 3);
    return f;
}
void Begin(VkRecording::Reuse reuse = VkRecording::Reuse::Serial)
{
    VkRecording::Recordings().Begin(3, reuse);
    assert(PairCapture::RequestVk(nullptr));
}
int main()
{
    using namespace PairCapture;
    // Never requested: no allocation/commands, not even a session or manager operation is needed.
    for (int i = 0; i < 100; ++i)
        PollVkLocked();
    assert(!fake::created && !fake::copies);
    VkRecording::Recordings().Allocate(1, 2, { 3 });
    Begin(VkRecording::Reuse::Simultaneous);
    assert(!BeginVkLocked(Frame()));
    assert(!fake::created && !fake::copies);
    Begin();
    fake::padding = VkPairCapture::kBudgetBytes;
    assert(!BeginVkLocked(Frame()));
    fake::padding = 256;
    PollVkLocked();
    assert(fake::created == fake::destroyed && State().budget.Used() == 0 && !fake::copies);
    Begin();
    fake::failAlloc = true;
    assert(!BeginVkLocked(Frame()));
    fake::failAlloc = false;
    PollVkLocked();
    assert(fake::created == fake::destroyed && State().budget.Used() == 0);
    Begin();
    assert(BeginVkLocked(Frame()));
    assert(fake::copies == 1);
    CommitVkLocked((VkCommandBuffer) 3);
    fake::Submit();
    PollVkLocked();
    assert(!fake::job); // even an executed marker cannot permit a read before retirement
    fake::Submit();     // legal serial replay of the exact pair; no descriptor mutation
    fake::Reset();
    PollVkLocked();
    assert(fake::job);
    std::vector<uint8_t> pixels[2];
    assert(fake::job->prepare(pixels));
    assert(pixels[0][0] == 1 && pixels[1][0] == 2);
    fake::job->finish(true, nullptr, "pair");
    fake::job.reset();
    PollVkLocked();
    assert(State().budget.Used() == 0 && fake::created == fake::destroyed);
    // Black pixels are valid when the recording executed; the marker is independent of image data.
    Begin();
    assert(BeginVkLocked(Frame()));
    CommitVkLocked((VkCommandBuffer) 3);
    fake::Submit();
    for (auto& [key, mem] : fake::memory)
    {
        (void) key;
        std::fill(mem.begin(), mem.begin() + 64, 0);
    }
    fake::Reset();
    PollVkLocked();
    assert(fake::job);
    assert(fake::job->prepare(pixels));
    assert(pixels[0][0] == 0 && pixels[1][0] == 0);
    fake::job->finish(true, nullptr, "black");
    fake::job.reset();
    PollVkLocked();
    assert(State().budget.Used() == 0);
    // Complete recording discarded without submission: no writer, no black-image heuristic.
    Begin();
    assert(BeginVkLocked(Frame()));
    CommitVkLocked((VkCommandBuffer) 3);
    fake::Reset();
    PollVkLocked();
    assert(!fake::job && StatusVk().reason == kReasonDiscarded && State().budget.Used() == 0);
    // Failed NR evaluation after original: retained until recording is retired, never handed off.
    Begin();
    assert(BeginVkLocked(Frame()));
    CancelVkLocked();
    PollVkLocked();
    assert(State().budget.Used() != 0);
    fake::Reset();
    PollVkLocked();
    assert(!fake::job && State().budget.Used() == 0);
    // Writer queue vs destruction: invalidation must precede Forget, prepare must not touch a dead mapping.
    Begin();
    assert(BeginVkLocked(Frame()));
    CommitVkLocked((VkCommandBuffer) 3);
    fake::Submit();
    fake::Reset();
    PollVkLocked();
    assert(fake::job);
    const auto destroys = fake::destroyed;
    DeviceGoneVkLocked((VkDevice) 1, false);
    VkRecording::Recordings().ForgetDevice(1);
    assert(fake::destroyed == destroys);
    assert(!fake::job->prepare(pixels));
    fake::job->finish(false, kReasonDiscarded, {});
    fake::job.reset();
    PollVkLocked();
    assert(State().budget.Used() > 0); // abandoned GPU bytes remain charged
    // Fake driver reclaims device children on destruction (production intentionally abandoned them).
    fake::buffers.clear();
    fake::memory.clear();
    std::cout << "PASS: production Vulkan backend allocation, split snapshots, replay, discard, cancellation, "
                 "writer/device loss\n";
}
