// Real-GPU endpoint-error test for SynthMotion::Estimator_Vk (retoque/shaders/synth_motion/SynthMotion_Vk.*), the
// Vulkan twin of the D3D12 estimator, on a real Vulkan device under Wine (winevulkan to the host's driver), with the
// Khronos validation layer's core and synchronization checks on.
//
// The frames, the sequences and the scores are tests/synth-motion-d3d12's (sequences.h), so the Vulkan estimator is
// held to the same pixels against the same truth, and run.py judges the report with that suite's thresholds. Nothing
// below the estimator's public interface is mocked: the production VkPipelines, VkImages and Estimator_Vk, called the
// way the Vulkan present host calls them (DlssNrFeature_Vk.cpp) -- the images made and initialised in a command buffer
// of their own and waited for, a ticket per frame, ConfirmExecuted on submit, AbandonRecording for a frame never
// submitted, Completed once its fence is seen.
//
// Convention under test (the interface's promise): the field holds, per colour pixel, the displacement from the
// current frame to the previous one, in colour pixels, +x right, +y down. Content moving (+dx, +dy) reads (-dx, -dy).
#include "pch.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <stdexcept>

#include "SynthMotion_Vk.h"

namespace
{
FILE* g_report = nullptr;
const char* g_reportPath = "synth-motion-report.json";

// What run.py's judged run does not use (README, "Investigating"): a subset of the sequences, the sequences several
// times over with every later round compared with the first, and frames with nothing of the host's between them.
const char* g_only = nullptr; // --only a,b
int g_rounds = 1;             // --repeat N
int g_round = 0;
bool g_fast = false;   // --fast: each frame rendered once into a buffer of its own; no hash, no score
int g_paceMs = 0;      // --pace N: the host idle this long before each frame
uint32_t g_depth = 0;  // --depth N: frames in flight, instead of each sequence's own
int g_inFlight = 0;    // frames still on the GPU when their objects came round again
int g_roundFrames = 0; // frames compared with the first round's
int g_roundMismatches = 0;
#ifdef SYNTH_SCD_STATE
int g_unnormalised = 0; // frames that left a previous histogram that does not sum to one
double g_sumLow = 1e30, g_sumHigh = -1e30;
#endif

// What a frame left in the estimator's scene-change output the first time round.
std::map<std::string, std::vector<std::array<uint32_t, 3>>> g_firstRound;
// Each frame's scene-change value by FidelityFX's formula, on the host (sequences.h, SceneChangeReference).
std::map<std::string, std::vector<float>> g_reference;

// FNV-1a: which bytes these were, not a checksum against anyone.
uint64_t ContentHash(const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

bool Selected(const char* name)
{
    if (g_only == nullptr)
        return true;
    const size_t length = std::strlen(name);
    for (const char* at = g_only; *at;)
    {
        const char* end = std::strchr(at, ',');
        const size_t n = end ? (size_t) (end - at) : std::strlen(at);
        if (n == length && std::strncmp(at, name, n) == 0)
            return true;
        at += n + (end ? 1 : 0);
    }
    return false;
}

std::atomic<unsigned> g_validationErrors { 0 };
std::atomic<unsigned> g_wineInjected { 0 };
std::atomic<bool> g_validationActive { false };
std::atomic<bool> g_validationProbe { false };
std::atomic<bool> g_syncProbe { false };
std::atomic<bool> g_syncActive { false };
std::string g_firstValidation;

void Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}

// A failure ends the process without unwinding GPU-owned objects, as the D3D12 harness does.
[[noreturn]] void Fail(const std::string& what)
{
    Log("FAIL: %s", what.c_str());
    std::fflush(stdout);
    ExitProcess(1);
}

// The device cannot run the estimator (SynthMotion::VkUnsupported): not a failure of the estimator; run.py says SKIP.
[[noreturn]] void Skip(const std::string& why)
{
    std::string reason = why;
    for (char& c : reason)
        c = (c == '"' || c == '\\') ? '\'' : c;
    FILE* report = nullptr;
    if (fopen_s(&report, g_reportPath, "w") == 0 && report != nullptr)
    {
        std::fprintf(report, "{\"source\": \"ffx\", \"api\": \"vulkan\", \"skipped\": \"%s\", \"sequences\": []}\n",
                     reason.c_str());
        std::fclose(report);
    }
    Log("SYNTH-MOTION-VK SKIP: %s", reason.c_str());
    ExitProcess(0);
}

void Check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        Fail(std::string(what) + " returned " + std::to_string(result));
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    const char* text = data->pMessage ? data->pMessage : "";
    const char* id = data->pMessageIdName ? data->pMessageIdName : "?";
    if (g_validationProbe && !std::strcmp(id, "VUID-VkFenceCreateInfo-flags-parameter"))
    {
        g_validationActive = true;
        Log("VALIDATION PROBE (expected): %s", text);
        return VK_TRUE;
    }
    if (g_syncProbe && !std::strncmp(id, "SYNC-HAZARD-", 12))
    {
        g_syncActive = true;
        Log("SYNCHRONIZATION PROBE (expected): %s | %s", id, text);
        return VK_FALSE;
    }
    // Wine's own VK_EXT_external_memory_dma_buf on the host-side device (tests/vulkan-present-host, harness.cpp).
    if (!std::strcmp(id, "VUID-vkCreateDevice-ppEnabledExtensionNames-01387") &&
        std::strstr(text, "VK_EXT_external_memory_dma_buf") != nullptr &&
        std::strstr(text, "ppEnabledExtensionNames[0]") == nullptr)
    {
        ++g_wineInjected;
        return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        if (g_validationErrors++ == 0)
            g_firstValidation = std::string(id) + ": " + text;
        Log("VALIDATION: %s | %s", id, text);
    }
    return VK_FALSE;
}

// Procedural content, the sequences and the estimator's scoring: the D3D12 suite's, shared.
#include "../synth-motion-d3d12/sequences.h"

// ---------------------------------------------------------------------------------------------
// The device.
// ---------------------------------------------------------------------------------------------
struct Gpu
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    double tickNs = 0.0;
    uint64_t tickMask = ~0ull;
    std::string deviceName;

    void Init()
    {
        VkDebugUtilsMessengerCreateInfoEXT debug { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        debug.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        debug.pfnUserCallback = DebugMessage;

        const char* extensions[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
        VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "OptiScaler synthesized motion, Vulkan";
        app.apiVersion = VK_API_VERSION_1_1; // what the estimator needs, no more: the floor it is tested at
        VkInstanceCreateInfo info { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        info.pNext = &debug;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = extensions;
        Check(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
        auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createDebug == nullptr)
            Fail("no debug messenger entry point");
        Check(createDebug(instance, &debug, nullptr, &messenger), "vkCreateDebugUtilsMessengerEXT");

        uint32_t count = 0;
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        for (VkPhysicalDevice candidate : devices)
        {
            VkPhysicalDeviceProperties props {};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (physical == VK_NULL_HANDLE || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            {
                physical = candidate;
                deviceName = props.deviceName;
                tickNs = props.limits.timestampPeriod;
            }
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                break;
        }
        if (physical == VK_NULL_HANDLE)
            Fail("no Vulkan device");

        const std::string unsupported = SynthMotion::VkUnsupported(physical, app.apiVersion);
        if (!unsupported.empty())
            Skip("the estimator cannot run on " + deviceName + ": " + unsupported);

        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, props.data());
        for (uint32_t i = 0; i < families && family == UINT32_MAX; ++i)
        {
            if ((props[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                family = i;
        }
        if (family == UINT32_MAX)
            Fail("no graphics and compute queue family");
        const uint32_t validBits = props[family].timestampValidBits;
        tickMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1);

        // VK_KHR_push_descriptor, as the present host adds it for the model; nothing else, and no feature.
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char* deviceExtensions[] = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
        VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = deviceExtensions;
        Check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);

        VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        Check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocate { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(device, &allocate, &cmd), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        Check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
        VkQueryPoolCreateInfo queryInfo { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2;
        Check(vkCreateQueryPool(device, &queryInfo, nullptr, &timestamps), "vkCreateQueryPool");

        ProbeValidation();
        ProbeSynchronization();
    }

    // The synchronization checks' negative control: two clears of one image with no barrier between them must be
    // reported as a hazard. Recorded, never submitted.
    void ProbeSynchronization()
    {
        VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R32_UINT;
        info.extent = { 16, 16, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        Check(vkCreateImage(device, &info, nullptr, &image), "vkCreateImage (probe)");
        VkMemoryRequirements req {};
        vkGetImageMemoryRequirements(device, image, &req);
        VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = req.size;
        allocate.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(device, &allocate, nullptr, &memory), "vkAllocateMemory (probe)");
        Check(vkBindImageMemory(device, image, memory, 0), "vkBindImageMemory (probe)");

        Begin();
        VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        const VkClearColorValue zero {};
        g_syncProbe = true;
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &barrier.subresourceRange);
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &barrier.subresourceRange);
        g_syncProbe = false;
        EndWithoutSubmitting();
        Check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer (probe)");
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        if (!g_syncActive)
            Fail("synchronization validation negative control failed: two unordered clears were not reported");
    }

    // The negative control: an invalid fence flag must reach the messenger with its exact VUID.
    void ProbeValidation()
    {
        VkFenceCreateInfo invalid { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        invalid.flags = 0x80000000u;
        VkFence probe = VK_NULL_HANDLE;
        g_validationProbe = true;
        const VkResult result = vkCreateFence(device, &invalid, nullptr, &probe);
        g_validationProbe = false;
        if (probe != VK_NULL_HANDLE)
            vkDestroyFence(device, probe, nullptr);
        if (!g_validationActive || result != VK_ERROR_VALIDATION_FAILED_EXT)
            Fail("validation negative control failed: the invalid fence flag was not intercepted");
    }

    void Begin()
    {
        Check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
    }

    void SubmitAndWait()
    {
        Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        Check(vkResetFences(device, 1, &fence), "vkResetFences");
        Check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
        Check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000), "vkWaitForFences");
    }

    // Recorded, ended, never submitted: what the present host does with a frame it discards.
    void EndWithoutSubmitting() { Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer"); }

    double TimestampMs()
    {
        uint64_t ticks[2] = {};
        if (vkGetQueryPoolResults(device, timestamps, 0, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
            return 0.0;
        const uint64_t begin = ticks[0] & tickMask, end = ticks[1] & tickMask;
        return end > begin ? (double) (end - begin) * tickNs / 1e6 : 0.0;
    }

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
    {
        VkPhysicalDeviceMemoryProperties memory {};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        {
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        }
        Fail("no memory type");
    }

    struct HostBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* data = nullptr;
    };

    HostBuffer Buffer(VkDeviceSize size, VkBufferUsageFlags usage)
    {
        HostBuffer out;
        VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage;
        Check(vkCreateBuffer(device, &info, nullptr, &out.buffer), "vkCreateBuffer");
        VkMemoryRequirements req {};
        vkGetBufferMemoryRequirements(device, out.buffer, &req);
        VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = req.size;
        allocate.memoryTypeIndex =
            MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkAllocateMemory(device, &allocate, nullptr, &out.memory), "vkAllocateMemory");
        Check(vkBindBufferMemory(device, out.buffer, out.memory, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        Check(vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
        out.data = static_cast<uint8_t*>(mapped);
        return out;
    }

    void Free(HostBuffer& b)
    {
        vkDestroyBuffer(device, b.buffer, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
        b = HostBuffer {};
    }
};

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                  VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// ---------------------------------------------------------------------------------------------
// The report: the D3D12 suite's schema (its run.py judges it), without the HUD mask's fields.
// ---------------------------------------------------------------------------------------------
struct FrameResult
{
    int t = 0;
    bool recorded = false;
    bool abandoned = false;
    bool ready = false;
    bool sceneCutAfterRecord = false;
    bool sceneCutAfterConfirm = false;
    // A scene cut's evidence: which frame this was, what the estimator was given, what its scene-change pass made of
    // it and what the host then read (docs/linux-proton/synth-motion-false-scene-cut-20261120.md).
    uint64_t ticket = 0;
    int completedBeforeRecord = -1;           // the last frame seen complete when this one was recorded
    uint64_t inputHash = 0, gpuInputHash = 0; // the colour uploaded; the colour copied back after the estimator read it
    uint32_t scd[3] = {};       // the GPU's scene-change output after this frame: value, history bits, groups counted
    float scdReference = -1.0f; // the value by FidelityFX's formula on the host; -1 on the frame after a reset
    uint32_t readbackHistory[SynthMotion::kVkReadbackSlots] = {}; // the estimator's slots when the fence was seen
    double gpuMs = 0.0;
    UINT fieldW = 0, fieldH = 0;
    std::vector<Score> scores;
};

void WriteSequence(const Sequence& s, const std::vector<FrameResult>& frames, bool first)
{
    std::fprintf(g_report,
                 "%s\n  {\"name\": \"%s\", \"width\": %u, \"height\": %u, \"dx\": %d, \"dy\": %d, \"timing_only\": %s, "
                 "\"frames\": [",
                 first ? "" : ",", s.name, s.width, s.height, s.dx, s.dy, s.timingOnly ? "true" : "false");
    for (size_t i = 0; i < frames.size(); ++i)
    {
        const auto& f = frames[i];
        std::fprintf(
            g_report,
            "%s\n    {\"t\": %d, \"recorded\": %s, \"abandoned\": %s, \"ready\": %s, \"scene_cut_after_record\": "
            "%s, \"scene_cut_after_confirm\": %s, \"gpu_ms\": %.6f, \"field_width\": %u, \"field_height\": %u, "
            "\"field_format\": %d, ",
            i ? "," : "", f.t, f.recorded ? "true" : "false", f.abandoned ? "true" : "false",
            f.ready ? "true" : "false", f.sceneCutAfterRecord ? "true" : "false",
            f.sceneCutAfterConfirm ? "true" : "false", f.gpuMs, f.fieldW, f.fieldH, (int) SynthMotion::kVkMotionFormat);
        float value;
        std::memcpy(&value, &f.scd[0], sizeof(value));
        std::fprintf(g_report,
                     "\"ticket\": %llu, \"completed_before_record\": %d, \"input_hash\": \"%016llx\", "
                     "\"gpu_input_hash\": \"%016llx\", \"cut_metric\": %.9g, \"scd_reference\": %.9g, "
                     "\"cut_threshold\": %.2f, \"cut_history\": %u, \"scd_groups\": %u, \"readback_history\": [",
                     f.ticket, f.completedBeforeRecord, f.inputHash, f.gpuInputHash, value, f.scdReference,
                     kSceneChangeThreshold, f.scd[1], f.scd[2]);
        for (uint32_t slot = 0; slot < SynthMotion::kVkReadbackSlots; ++slot)
            std::fprintf(g_report, "%s%u", slot ? ", " : "", f.readbackHistory[slot]);
        std::fprintf(g_report, "], \"scores\": [");
        for (size_t j = 0; j < f.scores.size(); ++j)
        {
            const auto& sc = f.scores[j];
            std::fprintf(g_report,
                         "%s{\"region\": \"%s\", \"gt_x\": %.3f, \"gt_y\": %.3f, \"pixels\": %zu, \"epe_mean\": %.6f, "
                         "\"within_1px\": %.6f, \"over_half_px\": %.6f, \"median_x\": %.4f, \"median_y\": %.4f}",
                         j ? ", " : "", sc.region, sc.gx, sc.gy, sc.pixels, sc.epeMean, sc.within1, sc.overHalf,
                         sc.medianX, sc.medianY);
        }
        std::fprintf(g_report, "]}");
    }
    std::fprintf(g_report, "\n  ]}");
    std::fflush(g_report);
}

// ---------------------------------------------------------------------------------------------
// One sequence end to end.
// ---------------------------------------------------------------------------------------------

// IEEE half from a float in [0, 1] (the colour's values are bytes over 255): normal numbers rounded to nearest, and
// below the smallest normal, zero.
uint16_t ToHalf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
    if (value <= 0.0f || exponent <= 0)
        return 0;
    uint32_t mantissa = bits & 0x7FFFFF;
    uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    if ((mantissa & 0x1FFF) > 0x1000 || ((mantissa & 0x1FFF) == 0x1000 && (half & 1)))
        ++half;
    return static_cast<uint16_t>(half);
}

constexpr VkDeviceSize kScdBins = 256 * 9; // the scene-change histograms: nine strata of 256 bins

// --fast: each frame's colour, rendered once into a buffer of its own and uploaded from there every round.
std::map<std::string, std::vector<Gpu::HostBuffer>> g_rendered;

// One frame's own objects, so frames can be in flight together: the colour it uploads (R16G16B16A16_SFLOAT, read in
// GENERAL, as the present host hands the estimator its working colour), the host buffers, its command buffer and fence,
// and its timestamp pair. `field` holds the field, then the colour as the GPU has it after the estimator read it, then
// the estimator's scene-change output: all copied out in the frame's own command buffer, behind its fence.
struct FrameSlot
{
    VkImage colour = VK_NULL_HANDLE;
    VkDeviceMemory colourMemory = VK_NULL_HANDLE;
    VkImageView colourView = VK_NULL_HANDLE;
    Gpu::HostBuffer upload;
    Gpu::HostBuffer field;
#ifdef SYNTH_SCD_STATE
    Gpu::HostBuffer scdState; // the histogram, the previous histogram and the temp, as the frame left them
#endif
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    uint64_t ticket = 0;
    int t = -1;
};

// `depth` frames in flight: 1 waits for every frame before the next (Completed before the next Record, as a host seeing
// each fence at once); more submits them back to back and sees each fence only when its objects come round again, so
// the estimator's barriers between frames are what orders them, under the synchronization checks.
std::vector<FrameResult> RunSequence(Gpu& gpu, const SynthMotion::VkPipelines& pipelines, const Sequence& s,
                                     uint64_t& ticket, uint32_t depth)
{
    const VkDevice device = gpu.device;
    const VkDeviceSize pixels = (VkDeviceSize) s.width * s.height;
    const VkDeviceSize colourOffset = pixels * 4, scdOffset = pixels * 12; // in a slot's `field`

    std::vector<FrameSlot> slots(depth);
    VkQueryPool timestamps = VK_NULL_HANDLE;
    {
        VkQueryPoolCreateInfo queryInfo { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2 * depth;
        Check(vkCreateQueryPool(device, &queryInfo, nullptr, &timestamps), "vkCreateQueryPool");
    }
    for (FrameSlot& slot : slots)
    {
        VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        info.extent = { s.width, s.height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        Check(vkCreateImage(device, &info, nullptr, &slot.colour), "vkCreateImage colour");
        VkMemoryRequirements req {};
        vkGetImageMemoryRequirements(device, slot.colour, &req);
        VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = req.size;
        allocate.memoryTypeIndex = gpu.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(device, &allocate, nullptr, &slot.colourMemory), "vkAllocateMemory colour");
        Check(vkBindImageMemory(device, slot.colour, slot.colourMemory, 0), "vkBindImageMemory colour");
        VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = slot.colour;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = info.format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        Check(vkCreateImageView(device, &view, nullptr, &slot.colourView), "vkCreateImageView colour");
        slot.upload = gpu.Buffer(pixels * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        slot.field = gpu.Buffer(scdOffset + 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
#ifdef SYNTH_SCD_STATE
        slot.scdState = gpu.Buffer(kScdBins * 8 + 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
#endif
        VkCommandBufferAllocateInfo cmdInfo { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cmdInfo.commandPool = gpu.pool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(device, &cmdInfo, &slot.cmd), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        Check(vkCreateFence(device, &fenceInfo, nullptr, &slot.fence), "vkCreateFence");
    }

    // The estimator's images, made and initialised in a command buffer of their own, waited for: the builder's shape.
    SynthMotion::VkImages images;
    {
        std::string why;
        gpu.Begin();
        if (!SynthMotion::CreateVkImages(device, gpu.physical, s.width, s.height, gpu.cmd, images, why))
            Fail("CreateVkImages: " + why);
        gpu.SubmitAndWait();
    }

    SynthMotion::Estimator_Vk estimator;
    estimator.Restart();

    std::vector<FrameResult> results(s.frames);
    std::vector<uint8_t> rgba((size_t) pixels * 4);
    std::vector<uint16_t> halves((size_t) pixels * 4);
    int completedFrame = -1;

    // FidelityFX's scene-change value for each executed frame, from its bytes and the half floats they become. A
    // frame recorded and never executed is not one: the estimator's history is still the frame before's.
    SceneChangeReference reference;
    float channel[256];
    for (int b = 0; b < 256; ++b)
        channel[b] = HalfToFloat(ToHalf(b / 255.0f));
    auto& referenceValues = g_reference[s.name];
    referenceValues.resize(s.frames, -1.0f);
    const auto refer = [&](int t)
    {
        if (t == 0)
            reference.Reset();
        if (!(s.kind == Kind::Abandon && t == kAbandonFrame))
            referenceValues[t] = reference.Frame(rgba.data(), s.width, s.height, s.width * 4, channel);
    };

    // A frame's fence seen: its scene-change bit read (Completed), its time and its field read and scored.
    const auto finish = [&](FrameSlot& slot)
    {
        if (!slot.pending)
            return;
        if (vkGetFenceStatus(device, slot.fence) == VK_NOT_READY)
            ++g_inFlight;
        Check(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 10ull * 1000 * 1000 * 1000), "vkWaitForFences");
        slot.pending = false;

        FrameResult& fr = results[slot.t];
        // What Completed is about to read: the history bits of the estimator's eight slots.
        for (uint32_t i = 0; i < SynthMotion::kVkReadbackSlots; ++i)
            fr.readbackHistory[i] = images.readbackData[i * 4 + 1];
        estimator.Completed(slot.ticket, images);
        completedFrame = slot.t;

        std::memcpy(fr.scd, slot.field.data + scdOffset, sizeof(fr.scd));
        if (!g_fast)
        {
            // Copied out first: the buffer is the driver's host-visible memory, slow to read a byte at a time.
            std::memcpy(halves.data(), slot.field.data + colourOffset, static_cast<size_t>(pixels * 8));
            fr.gpuInputHash = ContentHash(halves.data(), static_cast<size_t>(pixels * 8));
            if (fr.gpuInputHash != fr.inputHash)
                Fail(std::string(s.name) + " t=" + std::to_string(slot.t) +
                     ": the colour on the GPU is not the frame that was uploaded");
        }

        std::string detail;
#ifdef SYNTH_SCD_STATE
        // An investigation build: a normalised histogram sums to one, whatever the content.
        {
            const uint32_t* state = reinterpret_cast<const uint32_t*>(slot.scdState.data);
            bool unnormalised = false;
            for (int stratum = 0; stratum < 9; ++stratum)
            {
                double sum = 0.0;
                for (int i = 0; i < 256; ++i)
                {
                    float bin;
                    std::memcpy(&bin, &state[kScdBins + stratum * 256 + i], sizeof(bin));
                    sum += bin;
                }
                g_sumLow = std::min(g_sumLow, sum);
                g_sumHigh = std::max(g_sumHigh, sum);
                unnormalised |= std::abs(sum - 1.0) > 1e-3;
                if (stratum == 0)
                    detail = ", previous histogram of stratum 0 sums to " + std::to_string(sum);
            }
            g_unnormalised += unnormalised ? 1 : 0;
        }
#endif

        // Every round after the first must leave the scene-change output the first one left, bit for bit: the same
        // frames through the same passes, whatever else the GPU was doing.
        if (g_rounds > 1)
        {
            auto& first = g_firstRound[s.name];
            first.resize(s.frames);
            if (g_round == 0)
                first[slot.t] = { fr.scd[0], fr.scd[1], fr.scd[2] };
            else
            {
                ++g_roundFrames;
                const auto& was = first[slot.t];
                if (fr.scd[0] != was[0] || (fr.scd[1] & 1u) != (was[1] & 1u) || fr.scd[2] != was[2])
                {
                    ++g_roundMismatches;
                    float now, before;
                    std::memcpy(&now, &fr.scd[0], sizeof(now));
                    std::memcpy(&before, &was[0], sizeof(before));
                    SYSTEMTIME clock;
                    GetLocalTime(&clock);
                    Log("ROUND MISMATCH %02u:%02u:%02u.%03u round %d %s t=%d: scene-change value %.6f (first round "
                        "%.6f, FidelityFX %.6f), history %u (first %u), groups %u%s",
                        clock.wHour, clock.wMinute, clock.wSecond, clock.wMilliseconds, g_round, s.name, slot.t, now,
                        before, fr.scdReference, fr.scd[1], was[1], fr.scd[2], detail.c_str());
                }
            }
        }
        if (g_fast)
            return;

        const uint32_t pair = static_cast<uint32_t>(&slot - slots.data());
        uint64_t ticks[2] = {};
        if (vkGetQueryPoolResults(device, timestamps, pair * 2, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            const uint64_t begin = ticks[0] & gpu.tickMask, end = ticks[1] & gpu.tickMask;
            fr.gpuMs = end > begin ? (double) (end - begin) * gpu.tickNs / 1e6 : 0.0;
        }

        if (!s.timingOnly)
        {
            Field field;
            field.width = images.width; // what the estimator made, not what was asked for
            field.height = images.height;
            field.x.resize((size_t) field.width * field.height);
            field.y.resize(field.x.size());
            for (size_t i = 0; i < field.x.size(); ++i)
            {
                uint16_t h[2];
                std::memcpy(h, slot.field.data + i * 4, 4);
                field.x[i] = HalfToFloat(h[0]);
                field.y[i] = HalfToFloat(h[1]);
            }
            fr.fieldW = field.width;
            fr.fieldH = field.height;
            const bool hudOnly = std::strncmp(s.name, "hud_", 4) == 0;
            if (slot.t > 0 && !hudOnly && field.width == s.width && field.height == s.height)
                fr.scores = ScoreFrame(s, slot.t, field);
        }

        std::string line;
        for (const auto& sc : fr.scores)
        {
            char buf[240];
            std::snprintf(buf, sizeof(buf),
                          " %s: epe %.3f <1px %.1f%% >0.5px %.1f%% median (%.2f, %.2f) gt (%.0f, %.0f)", sc.region,
                          sc.epeMean, sc.within1 * 100.0, sc.overHalf * 100.0, sc.medianX, sc.medianY, sc.gx, sc.gy);
            line += buf;
        }
        Log("%-16s t=%d ready=%d cut=%d gpu=%.3f ms%s", s.name, fr.t, fr.ready ? 1 : 0, fr.sceneCutAfterRecord ? 1 : 0,
            fr.gpuMs, line.c_str());
    };

    for (int t = 0; t < s.frames; ++t)
    {
        FrameSlot& slot = slots[t % depth];
        finish(slot);

        // The frame as bytes, then as the half floats the host's working colour holds.
        FrameResult& fr = results[t];
        VkBuffer uploadFrom = slot.upload.buffer;
        if (g_fast)
        {
            auto& rendered = g_rendered[s.name];
            rendered.resize(s.frames);
            if (rendered[t].buffer == VK_NULL_HANDLE)
            {
                RenderFrame(s, t, rgba.data(), s.width * 4);
                refer(t);
                rendered[t] = gpu.Buffer(pixels * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
                auto* into = reinterpret_cast<uint16_t*>(rendered[t].data);
                for (size_t i = 0; i < rgba.size(); ++i)
                    into[i] = ToHalf(rgba[i] / 255.0f);
            }
            uploadFrom = rendered[t].buffer;
        }
        else
        {
            RenderFrame(s, t, rgba.data(), s.width * 4);
            refer(t);
            for (size_t i = 0; i < rgba.size(); ++i)
                halves[i] = ToHalf(rgba[i] / 255.0f);
            fr.inputHash = ContentHash(halves.data(), static_cast<size_t>(pixels * 8));
            std::memcpy(slot.upload.data, halves.data(), static_cast<size_t>(pixels * 8));
        }
        if (g_paceMs > 0)
            Sleep((DWORD) g_paceMs);

        const VkCommandBuffer cmd = slot.cmd;
        Check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

        // The whole frame is rewritten: its old contents are not needed, so the layout it was left in is not either.
        ImageBarrier(cmd, slot.colour, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { s.width, s.height, 1 };
        vkCmdCopyBufferToImage(cmd, uploadFrom, slot.colour, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        ImageBarrier(cmd, slot.colour, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        fr.t = t;
        fr.completedBeforeRecord = completedFrame;
        fr.scdReference = referenceValues[t];
        const uint64_t frameTicket = ++ticket;
        fr.ticket = frameTicket;
        const uint32_t pair = t % depth;
        vkCmdResetQueryPool(cmd, timestamps, pair * 2, 2);
        // Once everything before it is done (the colour's upload above all, tens of megabytes from host memory at
        // 3440x1440): the estimator's own time, which its first barrier waits for anyway.
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, timestamps, pair * 2);
        fr.recorded =
            estimator.Record(cmd, pipelines, images, slot.colourView, VK_IMAGE_LAYOUT_GENERAL, t == 0, frameTicket);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, pair * 2 + 1);
        if (!fr.recorded)
            Fail("Record returned false");
        fr.ready = estimator.Ready();
        fr.sceneCutAfterRecord = estimator.SceneCut();

        // The field out to the host, into this frame's own buffer. Record left its writes visible to transfer reads.
        {
            VkBufferImageCopy out {};
            out.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            out.imageExtent = { images.width, images.height, 1 };
            vkCmdCopyImageToBuffer(cmd, images.motion.image, VK_IMAGE_LAYOUT_GENERAL, slot.field.buffer, 1, &out);
            // The evidence: the colour as the GPU has it now the estimator has read it, and what its scene-change
            // pass left for this frame, into the same buffer behind the same fence.
            if (!g_fast)
            {
                ImageBarrier(cmd, slot.colour, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                out.bufferOffset = colourOffset;
                out.imageExtent = { s.width, s.height, 1 };
                vkCmdCopyImageToBuffer(cmd, slot.colour, VK_IMAGE_LAYOUT_GENERAL, slot.field.buffer, 1, &out);
            }
            out.bufferOffset = scdOffset;
            out.imageExtent = { 3, 1, 1 };
            vkCmdCopyImageToBuffer(cmd, images.scdOutput.image, VK_IMAGE_LAYOUT_GENERAL, slot.field.buffer, 1, &out);
#ifdef SYNTH_SCD_STATE
            // An investigation build only (README, "Investigating"): the estimator's scene-change images made with
            // TRANSFER_SRC, which the shipped ones do not have.
            const VkImage state[] = { images.scdHistogram.image, images.scdPreviousHistogram.image,
                                      images.scdTemp.image };
            for (uint32_t i = 0; i < 3; ++i)
            {
                out.bufferOffset = kScdBins * 4 * i;
                out.imageExtent = { i < 2 ? (uint32_t) kScdBins : 3u, 1, 1 };
                vkCmdCopyImageToBuffer(cmd, state[i], VK_IMAGE_LAYOUT_GENERAL, slot.scdState.buffer, 1, &out);
            }
            VkBufferMemoryBarrier stateToHost { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            stateToHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            stateToHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            stateToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            stateToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            stateToHost.buffer = slot.scdState.buffer;
            stateToHost.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                 &stateToHost, 0, nullptr);
#endif
            VkBufferMemoryBarrier toHost { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.buffer = slot.field.buffer;
            toHost.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                 &toHost, 0, nullptr);
        }
        Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

        if (s.kind == Kind::Abandon && t == kAbandonFrame)
        {
            // Recorded, ended, never submitted: the estimator must not count this frame.
            estimator.AbandonRecording();
            fr.abandoned = true;
            Log("%-16s t=%d abandoned", s.name, t);
            continue;
        }

        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        Check(vkResetFences(device, 1, &slot.fence), "vkResetFences");
        Check(vkQueueSubmit(gpu.queue, 1, &submit, slot.fence), "vkQueueSubmit");
        estimator.ConfirmExecuted();
        slot.pending = true;
        slot.ticket = frameTicket;
        slot.t = t;
    }

    // The frames still in flight, oldest first.
    for (int t = std::max(0, s.frames - (int) depth); t < s.frames; ++t)
        finish(slots[t % depth]);

    // Every frame was waited for (the abandoned one never ran): everything is idle.
    Check(vkQueueWaitIdle(gpu.queue), "vkQueueWaitIdle");
    SynthMotion::DestroyVkImages(device, images);
    for (FrameSlot& slot : slots)
    {
        gpu.Free(slot.upload);
        gpu.Free(slot.field);
#ifdef SYNTH_SCD_STATE
        gpu.Free(slot.scdState);
#endif
        vkDestroyImageView(device, slot.colourView, nullptr);
        vkDestroyImage(device, slot.colour, nullptr);
        vkFreeMemory(device, slot.colourMemory, nullptr);
        vkFreeCommandBuffers(device, gpu.pool, 1, &slot.cmd);
        vkDestroyFence(device, slot.fence, nullptr);
    }
    vkDestroyQueryPool(device, timestamps, nullptr);
    return results;
}
} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc)
            g_reportPath = argv[++i];
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc)
            g_only = argv[++i];
        else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
            g_rounds = std::clamp(std::atoi(argv[++i]), 1, 1000000);
        else if (std::strcmp(argv[i], "--fast") == 0)
            g_fast = true;
        else if (std::strcmp(argv[i], "--pace") == 0 && i + 1 < argc)
            g_paceMs = std::clamp(std::atoi(argv[++i]), 0, 5000);
        else if (std::strcmp(argv[i], "--depth") == 0 && i + 1 < argc)
            g_depth = (uint32_t) std::clamp(std::atoi(argv[++i]), 1, 4);
        else
            Fail(std::string("unknown argument ") + argv[i]);
    }

    Gpu gpu;
    gpu.Init();
    Log("device up: %s, timestamp period %.3f ns", gpu.deviceName.c_str(), gpu.tickNs);

    SynthMotion::VkPipelines pipelines;
    {
        std::string why;
        if (!SynthMotion::CreateVkPipelines(gpu.device, pipelines, why))
            Fail("CreateVkPipelines: " + why);
    }

    // The D3D12 suite's sequences, without its HUD mask's own (hud_*): there is no HUD mask on Vulkan.
    const Sequence sequences[] = {
        { "pan_+1x", Kind::Pan, 1280, 720, 1, 0, 14 },
        { "pan_+1y", Kind::Pan, 1280, 720, 0, 1, 14 },
        { "pan_+4-4", Kind::Pan, 1280, 720, 4, -4, 14 },
        { "pan_-16x", Kind::Pan, 1280, 720, -16, 0, 14 },
        { "pan_-16y", Kind::Pan, 1280, 720, 0, -16, 14 },
        { "pan_+16+16", Kind::Pan, 1280, 720, 16, 16, 14 },
        { "pan_+48x", Kind::Pan, 1280, 720, 48, 0, 14 },
        { "pan_+48y", Kind::Pan, 1280, 720, 0, 48, 14 },
        { "overlay_+8x", Kind::Overlay, 1280, 720, 8, 0, 14, false, false },
        { "overlay_+8+8", Kind::Overlay, 1280, 720, 8, 8, 14, false, false },
        { "object", Kind::Object, 1280, 720, 8, 3, 14 },
        { "static", Kind::Static, 1280, 720, 0, 0, 14 },
        { "cut", Kind::Cut, 1280, 720, 4, 0, 18 },
        // A change FidelityFX's own formula puts under its threshold: no scene cut, whatever the GPU's scheduling.
        { "partial", Kind::Partial, 1280, 720, 4, 0, 18 },
        { "abandon", Kind::Abandon, 1280, 720, 4, 0, 14 },
        { "time_1080p", Kind::Hud, 1920, 1080, 4, 0, 32, true },
        { "time_3440", Kind::Hud, 3440, 1440, 4, 0, 32, true },
        // Three frames in flight, each fence seen only when its objects come round again: the estimator's barriers
        // between frames order them, and Completed arrives late, as in the present host.
        { "pan_+4x_inflight", Kind::Pan, 1280, 720, 4, 0, 14 },
        { "cut_inflight", Kind::Cut, 1280, 720, 4, 0, 18 },
    };

    if (fopen_s(&g_report, g_reportPath, "w") != 0 || g_report == nullptr)
        Fail("open report");
    std::fprintf(g_report,
                 "{\"source\": \"ffx\", \"api\": \"vulkan\", \"device\": \"%s\", \"lag_frames\": 0, "
                 "\"sequences\": [",
                 gpu.deviceName.c_str());

    uint64_t ticket = 0;
    int frames = 0;
    bool first = true;
    for (g_round = 0; g_round < g_rounds; ++g_round)
    {
        for (const auto& s : sequences)
        {
            if (!Selected(s.name))
                continue;
            // In flight too: the timing sequences, so the GPU is kept busy (and clocked up) as in a game, not idle
            // between frames.
            const uint32_t depth =
                std::strstr(s.name, "_inflight") != nullptr || std::strncmp(s.name, "time_", 5) == 0 ? 3u : 1u;
            const auto results = RunSequence(gpu, pipelines, s, ticket, g_depth ? g_depth : depth);
            frames += (int) results.size();
            if (g_round > 0)
                continue; // the report holds the first round; the later ones are compared with it
            WriteSequence(s, results, first);
            first = false;
        }
    }

    Check(vkDeviceWaitIdle(gpu.device), "vkDeviceWaitIdle");
    SynthMotion::DestroyVkPipelines(pipelines);
    std::string firstText = g_firstValidation;
    for (char& c : firstText)
        c = (c == '"' || c == '\\' || c < ' ') ? '\'' : c;
    std::fprintf(g_report,
                 "\n], \"validation_errors\": %u, \"first_validation\": \"%s\", \"validation_active\": %s, "
                 "\"sync_validation_active\": %s, \"wine_injected\": %u, \"rounds\": %d, \"round_frames\": %d, "
                 "\"round_mismatches\": %d, \"frames_in_flight\": %d",
                 g_validationErrors.load(), firstText.c_str(), g_validationActive ? "true" : "false",
                 g_syncActive ? "true" : "false", g_wineInjected.load(), g_rounds, g_roundFrames, g_roundMismatches,
                 g_inFlight);
#ifdef SYNTH_SCD_STATE
    std::fprintf(g_report, ", \"unnormalised_frames\": %d, \"previous_sum_low\": %.6f, \"previous_sum_high\": %.6f",
                 g_unnormalised, g_sumLow, g_sumHigh);
    Log("previous histograms: %d frame(s) left one that does not sum to 1; sums %.4f to %.4f", g_unnormalised, g_sumLow,
        g_sumHigh);
#endif
    std::fprintf(g_report, "}\n");
    std::fclose(g_report);
    g_report = nullptr;

    Log("SYNTH-MOTION-VK DONE sequences=%d frames=%d validation_errors=%u rounds=%d round_frames=%d "
        "round_mismatches=%d frames_in_flight=%d",
        (int) (sizeof(sequences) / sizeof(sequences[0])), frames, g_validationErrors.load(), g_rounds, g_roundFrames,
        g_roundMismatches, g_inFlight);
    if (!g_firstValidation.empty())
        Log("first validation error: %s", g_firstValidation.c_str());
    return 0;
}
