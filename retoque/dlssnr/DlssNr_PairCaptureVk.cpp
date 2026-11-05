#include "pch.h"
#include "DlssNr_PairCaptureVk.h"
#include "DlssNr_VkPairCapture.h"
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <shaders/dlssnr/precompile/DlssNr_PairCapture_Shader_Vk.h>
#include <chrono>
#include <cstring>
#include <array>

namespace DlssNr::PairCapture
{
namespace
{
using namespace VkPairCapture;
static_assert(VK_FORMAT_R8G8B8A8_UNORM == 37 && VK_FORMAT_B8G8R8A8_UNORM == 44 && VK_FORMAT_R16G16B16A16_SFLOAT == 97 &&
              VK_FORMAT_R32G32B32A32_SFLOAT == 109);
std::atomic<bool> used { false };
std::atomic<bool> active { false };
int64_t Now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};
struct Attempt
{
    VkFrame frame;
    Plan plan;
    std::shared_ptr<Evidence> evidence = std::make_shared<Evidence>();
    Buffer buffers[2];
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] {};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint64_t reserved = 0;
    uint64_t gpuBytes = 0;
    bool abandoned = false;
    bool pinned = false;
    bool destroyed = false;
    // No destructor calls Vulkan. The manager frees/abandons explicitly with g_vkMutex held.
};
struct Manager
{
    Budget budget;
    uint64_t serial = 0;
    std::vector<std::shared_ptr<Attempt>> attempts;
};
Manager& State()
{
    static auto* state = new Manager;
    return *state;
}

void Destroy(Attempt& a, bool abandon = false)
{
    if (a.destroyed)
        return;
    const auto d = a.frame.device;
    if (!abandon)
    {
        for (auto& b : a.buffers)
        {
            if (b.mapped)
                vkUnmapMemory(d, b.memory);
            if (b.buffer)
                vkDestroyBuffer(d, b.buffer, nullptr);
            if (b.memory)
                vkFreeMemory(d, b.memory, nullptr);
        }
        if (a.pipeline)
            vkDestroyPipeline(d, a.pipeline, nullptr);
        if (a.pipelineLayout)
            vkDestroyPipelineLayout(d, a.pipelineLayout, nullptr);
        if (a.pool)
            vkDestroyDescriptorPool(d, a.pool, nullptr);
        if (a.layout)
            vkDestroyDescriptorSetLayout(d, a.layout, nullptr);
    }
    for (auto& b : a.buffers)
        b = {};
    a.destroyed = true;
    a.abandoned = abandon;
    // The CPU staging reservation remains until the writer's last reference is gone.
}
void Collect()
{
    auto& m = State();
    for (auto it = m.attempts.begin(); it != m.attempts.end();)
    {
        auto& a = **it;
        if (it->use_count() == 1 && (!a.pinned || a.evidence->retired.load(std::memory_order_acquire) || a.destroyed))
        {
            Destroy(a);
            // Unknown device-loss reclamation is not permission to spend the same GPU bytes twice.
            m.budget.Release(a.reserved - (a.abandoned ? a.gpuBytes : 0));
            it = m.attempts.erase(it);
        }
        else
            ++it;
    }
}

bool Succeeded(Attempt& a, VkResult result)
{
    if (result == VK_ERROR_DEVICE_LOST)
        a.evidence->invalid.store(true, std::memory_order_release);
    return result == VK_SUCCESS;
}

bool MakeBuffer(Attempt& a, Buffer& b, const VkPhysicalDeviceMemoryProperties& props)
{
    VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = a.plan.dataBytes + sizeof(uint64_t);
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Succeeded(a, vkCreateBuffer(a.frame.device, &info, nullptr, &b.buffer)))
        return false;
    VkMemoryRequirements req {};
    vkGetBufferMemoryRequirements(a.frame.device, b.buffer, &req);
    if (!State().budget.Reserve(req.size))
        return false;
    a.reserved += req.size;
    a.gpuBytes += req.size;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
            type = i;
            break;
        }
    if (type == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo alloc { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (!Succeeded(a, vkAllocateMemory(a.frame.device, &alloc, nullptr, &b.memory)) ||
        !Succeeded(a, vkBindBufferMemory(a.frame.device, b.buffer, b.memory, 0)) ||
        !Succeeded(a, vkMapMemory(a.frame.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped)))
        return false;
    // This memory has never been exposed to a recording. Pixel contents are deliberately not a sentinel.
    const uint64_t zero = 0;
    std::memcpy(static_cast<uint8_t*>(b.mapped) + a.plan.dataBytes, &zero, sizeof(zero));
    return true;
}

bool Initialise(Attempt& a)
{
    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(a.frame.physical, &props);
    a.plan = VkPairCapture::PlanFor(a.frame.facts.width, a.frame.facts.height, a.frame.format, a.frame.facts.linearHdr,
                                    props.limits.maxStorageBufferRange);
    if (!a.plan || (uint64_t(a.frame.facts.width) + 7) / 8 > props.limits.maxComputeWorkGroupCount[0] ||
        (uint64_t(a.frame.facts.height) + 7) / 8 > props.limits.maxComputeWorkGroupCount[1])
        return false;
    const uint64_t staging = a.plan.dataBytes * 2 + uint64_t(a.frame.facts.width) * 32 + 256 * 1024;
    if (!State().budget.Reserve(staging))
        return false;
    a.reserved = staging; // writer staging, before either Vk allocation
    a.frame.facts.plan = a.plan.image;
    VkPhysicalDeviceMemoryProperties memory {};
    vkGetPhysicalDeviceMemoryProperties(a.frame.physical, &memory);
    if (!MakeBuffer(a, a.buffers[0], memory) || !MakeBuffer(a, a.buffers[1], memory))
        return false;
    VkDescriptorSetLayoutBinding bindings[2] {};
    bindings[0] = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo layout { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layout.bindingCount = 2;
    layout.pBindings = bindings;
    if (!Succeeded(a, vkCreateDescriptorSetLayout(a.frame.device, &layout, nullptr, &a.layout)))
        return false;
    VkPushConstantRange push { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pl { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &a.layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (!Succeeded(a, vkCreatePipelineLayout(a.frame.device, &pl, nullptr, &a.pipelineLayout)))
        return false;
    // Byte headers have no uint32 alignment guarantee: copy before passing pCode to the driver.
    std::vector<uint32_t> code(sizeof(dlssnr_pair_capture_spv) / 4);
    std::memcpy(code.data(), dlssnr_pair_capture_spv, sizeof(dlssnr_pair_capture_spv));
    VkShaderModuleCreateInfo si { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    si.codeSize = sizeof(dlssnr_pair_capture_spv);
    si.pCode = code.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    if (!Succeeded(a, vkCreateShaderModule(a.frame.device, &si, nullptr, &shader)))
        return false;
    VkComputePipelineCreateInfo pi { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pi.layout = a.pipelineLayout;
    pi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = shader;
    pi.stage.pName = "CSMain";
    const auto result = vkCreateComputePipelines(a.frame.device, VK_NULL_HANDLE, 1, &pi, nullptr, &a.pipeline);
    if (result != VK_ERROR_DEVICE_LOST)
        vkDestroyShaderModule(a.frame.device, shader, nullptr);
    if (!Succeeded(a, result))
        return false;
    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 },
                                      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 } };
    VkDescriptorPoolCreateInfo pool { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool.maxSets = 2;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    if (!Succeeded(a, vkCreateDescriptorPool(a.frame.device, &pool, nullptr, &a.pool)))
        return false;
    VkDescriptorSetLayout layouts[2] = { a.layout, a.layout };
    VkDescriptorSetAllocateInfo ds { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ds.descriptorPool = a.pool;
    ds.descriptorSetCount = 2;
    ds.pSetLayouts = layouts;
    if (!Succeeded(a, vkAllocateDescriptorSets(a.frame.device, &ds, a.sets)))
        return false;
    for (int i = 0; i < 2; ++i)
    {
        VkDescriptorImageInfo image { VK_NULL_HANDLE, a.frame.view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo buffer { a.buffers[i].buffer, 0, a.plan.dataBytes };
        VkWriteDescriptorSet writes[2] {};
        for (auto& w : writes)
        {
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = a.sets[i];
            w.descriptorCount = 1;
        }
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].pImageInfo = &image;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &buffer;
        vkUpdateDescriptorSets(a.frame.device, 2, writes, 0, nullptr);
    }
    return true;
}

Result InspectAttempt(const Attempt& a)
{
    return Inspect(*a.evidence,
                   [&]
                   {
                       uint64_t marker = 0;
                       if (!a.destroyed && a.buffers[1].mapped)
                           std::memcpy(&marker, static_cast<const uint8_t*>(a.buffers[1].mapped) + a.plan.dataBytes,
                                       sizeof(marker));
                       return marker;
                   });
}
void Snapshot(VkCommandBuffer cmd, Attempt& a, int which)
{
    VkImageMemoryBarrier image { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    image.oldLayout = image.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image.image = a.frame.image;
    image.subresourceRange = a.frame.range;
    image.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    image.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &image);
    // Serial recordings may be replayed: order new writes after all previous uses of this private buffer.
    VkBufferMemoryBarrier buffer { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    buffer.srcQueueFamilyIndex = buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer.buffer = a.buffers[which].buffer;
    buffer.size = VK_WHOLE_SIZE;
    buffer.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    buffer.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                         &buffer, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a.pipelineLayout, 0, 1, &a.sets[which], 0, nullptr);
    const uint32_t size[2] = { a.frame.facts.width, a.frame.facts.height };
    vkCmdPushConstants(cmd, a.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
    vkCmdDispatch(cmd, (size[0] + 7) / 8, (size[1] + 7) / 8, 1);
    image.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    image.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &image);
    if (which == 1)
        vkCmdUpdateBuffer(cmd, a.buffers[1].buffer, a.plan.dataBytes, sizeof(uint64_t), &a.evidence->cookie);
    buffer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    buffer.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &buffer, 0, nullptr);
}

struct Backend
{
    using Readback = std::shared_ptr<Attempt>;
    using Ticket = std::shared_ptr<Attempt>;
    using Payload = uint8_t;
    struct Recording
    {
        VkFrame frame;
        std::shared_ptr<Attempt> attempt;
    };
    bool Alloc(Recording& r, int which, Readback& out)
    {
        if (which == 0)
        {
            Collect();
            if (State().attempts.size() >= kMaxRetained)
                return false;
            r.attempt = std::make_shared<Attempt>();
            r.attempt->frame = std::move(r.frame);
            r.attempt->evidence->cookie = ++State().serial;
            State().attempts.push_back(r.attempt); // owns partial initialisation before any allocating operation
            if (!Initialise(*r.attempt))
            {
                if (r.attempt->evidence->invalid.load(std::memory_order_acquire))
                    DeviceGoneVkLocked(r.attempt->frame.device, false);
                else
                    Destroy(*r.attempt);
                return false;
            }
        }
        out = r.attempt;
        return true;
    }
    bool Track(Recording& r, Ticket& ticket)
    {
        auto& a = *r.attempt;
        if (!Admissible(a.frame.recording))
            return false;
        // Only the registry owns the token after this function. Attempt owns the separate evidence, not the token.
        auto pin = std::make_shared<Retirement>(a.evidence);
        if (!VkRecording::Recordings().Pin(a.frame.recording, { pin }))
            return false;
        a.pinned = true;
        ticket = r.attempt;
        return true;
    }
    void Copy(Recording& r, int which, Readback& a) { Snapshot(r.frame.cmd, *a, which); }
    bool Completed(const Ticket& a) { return InspectAttempt(*a) == Result::Readable; }
    bool Discarded(const Ticket& a) { return InspectAttempt(*a) == Result::Discarded; }
    bool Ready(const Ticket& a) { return a->destroyed || a->evidence->retired.load(std::memory_order_acquire); }
    Completion CompletionKind() { return Completion::VulkanRetired; }
    void Release(Readback& a) { a.reset(); }
    void Leak(Readback (&a)[2])
    {
        a[0].reset();
        a[1].reset();
    } // manager retains until retirement, never age-frees
    bool Hand(Readback (&a)[2], Payload&&, Completion);
};
Session<Backend>& SessionVk()
{
    static auto* session = new Session<Backend>;
    return *session;
}
bool Backend::Hand(Readback (&a)[2], Payload&&, Completion)
{
    auto attempt = a[0];
    auto job = std::make_shared<CpuJob>();
    job->frame = attempt->frame.facts;
    job->bytes = attempt->plan.dataBytes;
    job->stamp = attempt->frame.stamp;
    job->frame.api = "Vulkan";
    job->frame.originalDescription = "the foreign output view before encode";
    const char* format = attempt->frame.format == VK_FORMAT_R16G16B16A16_SFLOAT   ? "VK_FORMAT_R16G16B16A16_SFLOAT"
                         : attempt->frame.format == VK_FORMAT_R32G32B32A32_SFLOAT ? "VK_FORMAT_R32G32B32A32_SFLOAT"
                         : attempt->frame.format == VK_FORMAT_B8G8R8A8_UNORM      ? "VK_FORMAT_B8G8R8A8_UNORM"
                                                                                  : "VK_FORMAT_R8G8B8A8_UNORM";
    job->frame.sourceFormat = format;
    job->frame.representation = "sampled-RGBA32F-numeric";
    job->frame.recordingEpoch = attempt->frame.recording.epoch;
    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(attempt->frame.physical, &props);
    job->gpu = props.deviceName;
    job->driver = std::to_string(props.driverVersion);
    job->prepare = [attempt](std::vector<uint8_t>(&pixels)[2])
    {
        for (auto& p : pixels)
            p.resize(static_cast<size_t>(attempt->plan.dataBytes)); // outside g_vkMutex
        std::lock_guard<std::mutex> lock(PairCaptureVkMutex());
        if (InspectAttempt(*attempt) != Result::Readable || attempt->destroyed)
            return false;
        for (int i = 0; i < 2; ++i)
            std::memcpy(pixels[i].data(), attempt->buffers[i].mapped, pixels[i].size());
        return true;
    };
    job->finish = [](bool ok, const char* reason, std::string stem)
    {
        std::lock_guard<std::mutex> lock(PairCaptureVkMutex());
        SessionVk().Finished(ok, reason, std::move(stem));
        ReleaseIfIdle(Api::Vulkan, SessionVk());
    };
    return EnqueueCpuPair(std::move(job));
}
} // namespace

bool RequestVk(const char** why)
{
    used.store(true, std::memory_order_release);
    if (SessionVk().Request(Now()))
    {
        active.store(true, std::memory_order_release);
        return true;
    }
    if (why)
        *why = SessionVk().GaveUp() ? kReasonNoCompletion : kReasonBusy;
    Relinquish(Api::Vulkan);
    return false;
}
PairCaptureStatus StatusVk()
{
    PairCaptureStatus status;
    status.directoryFailed = DirectoryFailed();
    if (!used.load(std::memory_order_acquire))
        return status;
    auto& s = SessionVk();
    s.ExpireIfStale(Now());
    status.phase = s.Current();
    status.gaveUp = s.GaveUp();
    const auto outcome = s.LastOutcome();
    status.ok = outcome.ok;
    status.reason = outcome.reason;
    status.lastStem = outcome.stem;
    ReleaseIfIdle(Api::Vulkan, s);
    return status;
}
bool WantedVk() { return used.load(std::memory_order_relaxed) && SessionVk().Current() == Phase::Requested; }
void PollVkLocked()
{
    if (!active.load(std::memory_order_relaxed))
        return;
    auto& s = SessionVk();
    if (s.Busy())
    {
        Backend backend;
        try
        {
            s.Poll(backend, Now());
        }
        catch (...)
        {
            s.Abandon(kReasonInternal);
        }
    }
    Collect();
    std::lock_guard<std::mutex> broker(brokerMutex);
    if (s.Current() == Phase::Idle)
    {
        Relinquish(Api::Vulkan);
        if (State().attempts.empty())
            active.store(false, std::memory_order_release);
    }
}
bool BeginVkLocked(VkFrame frame)
{
    if (!WantedVk())
        return false;
    Backend backend;
    Backend::Recording r;
    r.frame = std::move(frame);
    r.frame.stamp = FormatUtc(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    auto& s = SessionVk();
    const char* refusal = r.frame.refusal;
    if (DirectoryFailed())
        refusal = kReasonDirectory;
    if (!VkPairCapture::PlanFor(1, 1, r.frame.format, r.frame.facts.linearHdr, 16))
        refusal = ImageFile::kRefusalFormat;
    if (!Admissible(r.frame.recording))
        refusal = kReasonUntracked;
    if (r.frame.range.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || r.frame.range.levelCount != 1 ||
        r.frame.range.layerCount != 1 || r.frame.view == VK_NULL_HANDLE || r.frame.image == VK_NULL_HANDLE)
        refusal = kReasonSurface;
    try
    {
        if (!s.BeginRecord(backend, r, 0, refusal))
        {
            ReleaseIfIdle(Api::Vulkan, s);
            return false;
        }
        // Alloc moves frame into Attempt; preserve the command handle for both snapshots.
        r.frame.cmd = r.attempt->frame.cmd;
        s.RecordOriginal(backend, r);
        return true;
    }
    catch (...)
    {
        s.Abandon(kReasonInternal);
        return false;
    }
}
void CommitVkLocked(VkCommandBuffer cmd)
{
    Backend backend;
    Backend::Recording r;
    r.frame.cmd = cmd;
    SessionVk().CommitRecord(backend, r);
}
void CancelVkLocked()
{
    if (used.load(std::memory_order_relaxed))
        SessionVk().Abandon(kReasonDiscarded);
}
void DeviceGoneVkLocked(VkDevice device, bool drained)
{
    if (!used.load(std::memory_order_relaxed))
        return;
    for (auto& a : State().attempts)
        if (a->frame.device == device)
        {
            a->evidence->invalid.store(true, std::memory_order_release); // BEFORE administrative lease release
            Destroy(*a, !drained); // writer validation/copy shares g_vkMutex; no stale mapping can escape
        }
    PollVkLocked();
}
} // namespace DlssNr::PairCapture
