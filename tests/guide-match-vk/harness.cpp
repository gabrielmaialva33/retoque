// The Vulkan guide resample (retoque/shaders/dlssnr/DlssNr_GuideMatch_Vk.*,
// docs/dlssnr/design/reduced-scale-guides-vk.md) on a real Vulkan device under Wine (winevulkan to the host's driver),
// with the Khronos validation layer's core and synchronization checks on, each proved active by a negative control.
//
// The production DlssNr_GuideMatch_Vk, called the way the Vulkan after-upscale route calls it (DlssNrFeature_Vk.cpp):
// made once on the device, the game's depth and motion uploaded into images of their own formats and left in
// SHADER_READ_ONLY_OPTIMAL (as NGX takes them), the outputs made as the route makes them (R32_SFLOAT and R32G32_SFLOAT,
// the route's usage, never initialised), Record into a command buffer, the outputs read back. Analytic content:
// every source texel holds a value of its own, so each output texel names the texel it was read from, and it must be
// exactly DlssNr::GuideMatch::PointSource's prediction, bit for bit. One sequence runs 20 frames, three in flight, the
// inputs changing every frame and the outputs shared, as in the route: the pass's barriers alone order one frame's
// write after the last one's read, and its descriptor ring wraps.
#include "pch.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#include <shaders/dlssnr/DlssNr_GuideMatch_Vk.h>

namespace
{
using DlssNr::GuideMatch::PointSource;
using DlssNr::GuideMatch::Region;

FILE* g_report = nullptr;
const char* g_reportPath = "guide-match-report.json";

std::atomic<unsigned> g_validationErrors { 0 };
std::atomic<unsigned> g_wineInjected { 0 };
std::atomic<bool> g_validationActive { false };
std::atomic<bool> g_validationProbe { false };
std::atomic<bool> g_syncProbe { false };
std::atomic<bool> g_syncActive { false };

void Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}

[[noreturn]] void Fail(const std::string& what)
{
    Log("FAIL: %s", what.c_str());
    ExitProcess(1);
}

[[noreturn]] void Skip(const std::string& why)
{
    std::string reason = why;
    for (char& c : reason)
        c = (c == '"' || c == '\\') ? '\'' : c;
    FILE* report = nullptr;
    if (fopen_s(&report, g_reportPath, "w") == 0 && report != nullptr)
    {
        std::fprintf(report, "{\"skipped\": \"%s\", \"cases\": []}\n", reason.c_str());
        std::fclose(report);
    }
    Log("GUIDE-MATCH-VK SKIP: %s", reason.c_str());
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
        Log("SYNCHRONIZATION PROBE (expected): %s", id);
        return VK_FALSE;
    }
    // Extensions winevulkan adds to the host-side device it creates (VK_EXT_external_memory_dma_buf without its
    // dependency; tests/vulkan-present-host, harness.cpp): this application enables no device extension at all, so a
    // missing-dependency report about one is Wine's.
    if (!std::strcmp(id, "VUID-vkCreateDevice-ppEnabledExtensionNames-01387"))
    {
        ++g_wineInjected;
        return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        ++g_validationErrors;
        Log("VALIDATION: %s | %s", id, text);
    }
    return VK_FALSE;
}

// Exact half <-> float for the values used here (integers and halves well inside half's range).
uint16_t ToHalf(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = (int32_t) ((bits >> 23) & 0xFF) - 127 + 15;
    const uint32_t mantissa = bits & 0x7FFFFFu;
    if ((bits & 0x7FFFFFFFu) == 0)
        return (uint16_t) sign;
    if (exponent <= 0 || exponent >= 31 || (mantissa & 0x1FFFu) != 0)
        Fail("a motion value the half format cannot hold exactly");
    return (uint16_t) (sign | ((uint32_t) exponent << 10) | (mantissa >> 13));
}

std::string Exact(float value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.9g", value);
    return text;
}

uint32_t Bits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    return bits;
}

struct Gpu
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    VkCommandPool pool = VK_NULL_HANDLE;
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
        app.pApplicationName = "Retoque guide resample, Vulkan";
        // DLSS's Vulkan path needs 1.1 (its programming guide, 2.2), so the route never runs below it. The pass needs
        // nothing past 1.0, no extension and no feature.
        app.apiVersion = VK_API_VERSION_1_1;
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
            }
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                break;
        }
        if (physical == VK_NULL_HANDLE)
            Fail("no Vulkan device");

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

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        Check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);

        VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        Check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");

        ProbeValidation();
        ProbeSynchronization();
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

    VkCommandBuffer CommandBuffer()
    {
        VkCommandBufferAllocateInfo allocate { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        Check(vkAllocateCommandBuffers(device, &allocate, &cmd), "vkAllocateCommandBuffers");
        return cmd;
    }

    void Begin(VkCommandBuffer cmd)
    {
        Check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
    }

    void Submit(VkCommandBuffer cmd, VkFence fence)
    {
        Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        Check(vkResetFences(device, 1, &fence), "vkResetFences");
        Check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
    }

    void Wait(VkFence fence)
    {
        Check(vkWaitForFences(device, 1, &fence, VK_TRUE, 20ull * 1000 * 1000 * 1000), "vkWaitForFences");
    }

    VkFence Fence()
    {
        VkFenceCreateInfo info { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence fence = VK_NULL_HANDLE;
        Check(vkCreateFence(device, &info, nullptr, &fence), "vkCreateFence");
        return fence;
    }

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

    // Two clears of one image with no barrier between them must be reported as a hazard. Recorded, never submitted.
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

        VkCommandBuffer cmd = CommandBuffer();
        Begin(cmd);
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
        Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer (probe)");
        vkFreeCommandBuffers(device, pool, 1, &cmd);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        if (!g_syncActive)
            Fail("synchronization validation negative control failed: two unordered clears were not reported");
    }
};

Gpu g_gpu;

struct HostBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* data = nullptr;
    VkDeviceSize size = 0;
};

HostBuffer Buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
    HostBuffer out;
    out.size = size;
    VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size;
    info.usage = usage;
    Check(vkCreateBuffer(g_gpu.device, &info, nullptr, &out.buffer), "vkCreateBuffer");
    VkMemoryRequirements req {};
    vkGetBufferMemoryRequirements(g_gpu.device, out.buffer, &req);
    VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocate.allocationSize = req.size;
    allocate.memoryTypeIndex = g_gpu.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(g_gpu.device, &allocate, nullptr, &out.memory), "vkAllocateMemory (buffer)");
    Check(vkBindBufferMemory(g_gpu.device, out.buffer, out.memory, 0), "vkBindBufferMemory");
    void* mapped = nullptr;
    Check(vkMapMemory(g_gpu.device, out.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    out.data = static_cast<uint8_t*>(mapped);
    return out;
}

void Free(HostBuffer& b)
{
    vkDestroyBuffer(g_gpu.device, b.buffer, nullptr);
    vkFreeMemory(g_gpu.device, b.memory, nullptr);
    b = HostBuffer {};
}

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags allAspects = 0;    // what a barrier covers
    VkImageAspectFlags viewAspect = 0;    // what the view, and so the shader, sees
    VkImageSubresourceRange viewRange {}; // as a game would hand it to NGX
    uint32_t width = 0, height = 0;
};

bool IsDepth(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

Image MakeImage(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage)
{
    Image img;
    img.format = format;
    img.width = width;
    img.height = height;
    img.allAspects = format == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                     : IsDepth(format)                      ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                            : VK_IMAGE_ASPECT_COLOR_BIT;
    img.viewAspect = IsDepth(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { width, height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(g_gpu.device, &info, nullptr, &img.image), "vkCreateImage");
    VkMemoryRequirements req {};
    vkGetImageMemoryRequirements(g_gpu.device, img.image, &req);
    VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocate.allocationSize = req.size;
    allocate.memoryTypeIndex = g_gpu.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(g_gpu.device, &allocate, nullptr, &img.memory), "vkAllocateMemory (image)");
    Check(vkBindImageMemory(g_gpu.device, img.image, img.memory, 0), "vkBindImageMemory");
    img.viewRange = { img.viewAspect, 0, 1, 0, 1 };
    VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = img.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = img.viewRange;
    Check(vkCreateImageView(g_gpu.device, &view, nullptr, &img.view), "vkCreateImageView");
    return img;
}

void FreeImage(Image& img)
{
    vkDestroyImageView(g_gpu.device, img.view, nullptr);
    vkDestroyImage(g_gpu.device, img.image, nullptr);
    vkFreeMemory(g_gpu.device, img.memory, nullptr);
    img = Image {};
}

void Barrier(VkCommandBuffer cmd, const Image& img, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
             VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img.image;
    barrier.subresourceRange = { img.allAspects, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

uint32_t TexelBytes(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: // the depth aspect's copy layout: one 32-bit float per texel
        return 4;
    case VK_FORMAT_R32G32_SFLOAT:
        return 8;
    default:
        Fail("a format the harness does not upload");
    }
}

// The game's buffers this frame. Depth: every texel a value of its own in [0, 1) (a copy into D32_SFLOAT must stay
// there). Motion: x and y each name their texel's column and row, shifted by the frame, exactly representable in the
// format (halves well under 1024 for R16G16_SFLOAT).
float DepthAt(const Image& img, uint32_t x, uint32_t y, uint32_t frame)
{
    const uint64_t index = (uint64_t) y * img.width + x + frame * 7ull;
    return (float) ((double) index / (double) ((uint64_t) img.width * img.height + 1024));
}

void MotionAt(const Image& img, uint32_t x, uint32_t y, uint32_t frame, float& mx, float& my)
{
    if (img.format == VK_FORMAT_R16G16_SFLOAT)
    {
        mx = (float) ((int) ((x + frame) % 1000) - 500) + 0.5f * (float) (y & 1);
        my = (float) ((int) ((y + 3 * frame) % 1000) - 500) - 0.5f * (float) (x & 1);
    }
    else
    {
        mx = (float) x + 0.25f + (float) frame;
        my = -((float) y + 0.5f) - (float) frame;
    }
}

void Fill(const Image& img, HostBuffer& staging, uint32_t frame, bool depth)
{
    const uint32_t bytes = TexelBytes(img.format);
    if (staging.size < (VkDeviceSize) img.width * img.height * bytes)
        Fail("staging buffer too small");
    for (uint32_t y = 0; y < img.height; ++y)
        for (uint32_t x = 0; x < img.width; ++x)
        {
            uint8_t* at = staging.data + ((size_t) y * img.width + x) * bytes;
            if (depth)
            {
                const float d = DepthAt(img, x, y, frame);
                std::memcpy(at, &d, 4);
            }
            else
            {
                float mx = 0, my = 0;
                MotionAt(img, x, y, frame, mx, my);
                if (img.format == VK_FORMAT_R16G16_SFLOAT)
                {
                    const uint16_t h[2] = { ToHalf(mx), ToHalf(my) };
                    std::memcpy(at, h, 4);
                }
                else
                {
                    const float f[2] = { mx, my };
                    std::memcpy(at, f, 8);
                }
            }
        }
}

// Upload, and leave the image in SHADER_READ_ONLY_OPTIMAL, readable by a compute shader, as a game hands it to NGX.
void Upload(VkCommandBuffer cmd, const Image& img, const HostBuffer& staging, bool first)
{
    Barrier(cmd, img, first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, first ? 0 : VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region {};
    region.imageSubresource = { img.viewAspect, 0, 0, 1 };
    region.imageExtent = { img.width, img.height, 1 };
    vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

// What the model would read, copied out: the pass leaves its outputs in GENERAL with its writes made visible to shader
// reads; a copy needs its own transfer-read dependency.
void Readback(VkCommandBuffer cmd, const Image& out, const HostBuffer& into)
{
    Barrier(cmd, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { out.width, out.height, 1 };
    vkCmdCopyImageToBuffer(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, into.buffer, 1, &region);
    VkBufferMemoryBarrier toHost { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = into.buffer;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0,
                         nullptr);
}

struct Case
{
    const char* name;
    VkFormat depthFormat;
    uint32_t depthAllocW, depthAllocH;
    Region depth;
    VkFormat motionFormat;
    uint32_t motionAllocW, motionAllocH;
    Region motion;
    uint32_t outW, outH;
    uint32_t frames;   // 1, or a sequence
    uint32_t inFlight; // command buffers in flight
};

struct Outcome
{
    uint64_t texels = 0;
    uint64_t depthMismatches = 0;
    uint64_t motionMismatches = 0;
    std::string first;
};

// Every output texel against PointSource's prediction, bit for bit.
void Judge(const Case& c, const Image& depthIn, const Image& motionIn, uint32_t frame, const HostBuffer& depthOut,
           const HostBuffer& motionOut, Outcome& outcome)
{
    for (uint32_t j = 0; j < c.outH; ++j)
        for (uint32_t i = 0; i < c.outW; ++i)
        {
            ++outcome.texels;
            const uint32_t dx = PointSource(i, c.outW, c.depth.baseX, c.depth.width);
            const uint32_t dy = PointSource(j, c.outH, c.depth.baseY, c.depth.height);
            const uint32_t mx = PointSource(i, c.outW, c.motion.baseX, c.motion.width);
            const uint32_t my = PointSource(j, c.outH, c.motion.baseY, c.motion.height);

            float gotDepth = 0;
            std::memcpy(&gotDepth, depthOut.data + ((size_t) j * c.outW + i) * 4, 4);
            const float wantDepth = DepthAt(depthIn, dx, dy, frame);
            if (Bits(gotDepth) != Bits(wantDepth))
            {
                if (outcome.depthMismatches++ == 0 && outcome.first.empty())
                    outcome.first = "frame " + std::to_string(frame) + " depth (" + std::to_string(i) + "," +
                                    std::to_string(j) + ") = " + Exact(gotDepth) + ", want texel (" +
                                    std::to_string(dx) + "," + std::to_string(dy) + ") = " + Exact(wantDepth);
            }

            float got[2] = {};
            std::memcpy(got, motionOut.data + ((size_t) j * c.outW + i) * 8, 8);
            float want[2] = {};
            MotionAt(motionIn, mx, my, frame, want[0], want[1]);
            if (Bits(got[0]) != Bits(want[0]) || Bits(got[1]) != Bits(want[1]))
            {
                if (outcome.motionMismatches++ == 0 && outcome.first.empty())
                    outcome.first = "frame " + std::to_string(frame) + " motion (" + std::to_string(i) + "," +
                                    std::to_string(j) + ") = (" + Exact(got[0]) + "," + Exact(got[1]) +
                                    "), want texel (" + std::to_string(mx) + "," + std::to_string(my) + ") = (" +
                                    Exact(want[0]) + "," + Exact(want[1]) + ")";
            }
        }
}

bool Sampleable(VkFormat format)
{
    VkFormatProperties properties {};
    vkGetPhysicalDeviceFormatProperties(g_gpu.physical, format, &properties);
    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

std::string RunCase(DlssNr_GuideMatch_Vk& pass, const Case& c, Outcome& outcome)
{
    if (!Sampleable(c.depthFormat) || !Sampleable(c.motionFormat))
        return "skipped: the device cannot sample the case's formats";

    const VkImageUsageFlags inputUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const uint32_t slots = c.inFlight;

    // The outputs, shared across frames, made as the route makes them; never initialised.
    Image depthOut =
        MakeImage(DlssNr::GuideMatch::Vk::kDepthFormat, c.outW, c.outH, DlssNr::GuideMatch::Vk::kOutputUsage);
    Image motionOut =
        MakeImage(DlssNr::GuideMatch::Vk::kMotionFormat, c.outW, c.outH, DlssNr::GuideMatch::Vk::kOutputUsage);

    struct Slot
    {
        Image depth, motion;
        HostBuffer depthStaging, motionStaging, depthBack, motionBack;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        int64_t frame = -1; // the frame recorded into it, judged when it comes round again
        bool uploaded = false;
    };
    std::vector<Slot> ring(slots);
    for (Slot& s : ring)
    {
        s.depth = MakeImage(c.depthFormat, c.depthAllocW, c.depthAllocH, inputUsage);
        s.motion = MakeImage(c.motionFormat, c.motionAllocW, c.motionAllocH, inputUsage);
        s.depthStaging = Buffer((VkDeviceSize) c.depthAllocW * c.depthAllocH * TexelBytes(c.depthFormat),
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        s.motionStaging = Buffer((VkDeviceSize) c.motionAllocW * c.motionAllocH * TexelBytes(c.motionFormat),
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        s.depthBack = Buffer((VkDeviceSize) c.outW * c.outH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        s.motionBack = Buffer((VkDeviceSize) c.outW * c.outH * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        s.cmd = g_gpu.CommandBuffer();
        s.fence = g_gpu.Fence();
    }

    // The views as a game hands them: the route asks the pass whether it may bind them.
    if (const char* why =
            pass.InputRefusal(c.depthFormat, ring[0].depth.viewRange, c.motionFormat, ring[0].motion.viewRange))
        Fail(std::string(c.name) + ": the pass refuses the views: " + why);

    const auto judge = [&](Slot& s)
    {
        if (s.frame < 0)
            return;
        g_gpu.Wait(s.fence);
        Judge(c, s.depth, s.motion, (uint32_t) s.frame, s.depthBack, s.motionBack, outcome);
        s.frame = -1;
    };

    for (uint32_t frame = 0; frame < c.frames; ++frame)
    {
        Slot& s = ring[frame % slots];
        judge(s); // its last frame, three behind this one: only now are its buffers free

        Fill(s.depth, s.depthStaging, frame, true);
        Fill(s.motion, s.motionStaging, frame, false);
        g_gpu.Begin(s.cmd);
        Upload(s.cmd, s.depth, s.depthStaging, !s.uploaded);
        Upload(s.cmd, s.motion, s.motionStaging, !s.uploaded);
        s.uploaded = true;
        pass.Record(s.cmd, s.depth.view, s.motion.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, c.depth, c.motion,
                    depthOut.image, depthOut.view, motionOut.image, motionOut.view, c.outW, c.outH);
        Readback(s.cmd, depthOut, s.depthBack);
        Readback(s.cmd, motionOut, s.motionBack);
        g_gpu.Submit(s.cmd, s.fence);
        s.frame = frame;
    }

    for (Slot& s : ring)
        judge(s);

    Check(vkQueueWaitIdle(g_gpu.queue), "vkQueueWaitIdle");
    for (Slot& s : ring)
    {
        FreeImage(s.depth);
        FreeImage(s.motion);
        Free(s.depthStaging);
        Free(s.motionStaging);
        Free(s.depthBack);
        Free(s.motionBack);
        vkFreeCommandBuffers(g_gpu.device, g_gpu.pool, 1, &s.cmd);
        vkDestroyFence(g_gpu.device, s.fence, nullptr);
    }
    FreeImage(depthOut);
    FreeImage(motionOut);
    return {};
}
} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc)
            g_reportPath = argv[++i];
    }

    g_gpu.Init();
    Log("device up: %s", g_gpu.deviceName.c_str());

    if (const char* why = DlssNr_GuideMatch_Vk::Unsupported(g_gpu.physical, 1720, 720))
        Skip(std::string("the device cannot hold the resample's outputs: ") + why);

    DlssNr_GuideMatch_Vk pass;
    {
        std::string why;
        if (!pass.Create(g_gpu.device, g_gpu.physical, why))
            Fail("DlssNr_GuideMatch_Vk::Create: " + why);
    }

    // The shapes the route meets: DOOM Eternal at DLSS Quality and 0.5 (3440x1440, 2293x960 guides, a 1720x720 model),
    // with a depth region inside a larger allocation; display-resolution vectors; the vulkan-present-host route
    // harness's 960x540 under a 640x360 model; an exact 2:1 depth from a depth view of a depth/stencil image under an
    // offset half-float motion region; an extent that is not a multiple of eight; and DOOM's shape twenty frames deep
    // with three in flight.
    const Case cases[] = {
        { "doom_quality_half",
          VK_FORMAT_D32_SFLOAT,
          2304,
          968,
          { 5, 3, 2293, 960 },
          VK_FORMAT_R32G32_SFLOAT,
          2293,
          960,
          { 0, 0, 2293, 960 },
          1720,
          720,
          1,
          1 },
        { "display_motion",
          VK_FORMAT_R32_SFLOAT,
          2293,
          960,
          { 0, 0, 2293, 960 },
          VK_FORMAT_R32G32_SFLOAT,
          3440,
          1440,
          { 0, 0, 3440, 1440 },
          1720,
          720,
          1,
          1 },
        { "route_harness",
          VK_FORMAT_R32_SFLOAT,
          960,
          540,
          { 0, 0, 960, 540 },
          VK_FORMAT_R16G16_SFLOAT,
          960,
          540,
          { 0, 0, 960, 540 },
          640,
          360,
          1,
          1 },
        { "half_d32s8",
          VK_FORMAT_D32_SFLOAT_S8_UINT,
          1280,
          720,
          { 0, 0, 1280, 720 },
          VK_FORMAT_R16G16_SFLOAT,
          990,
          730,
          { 7, 9, 960, 720 },
          640,
          360,
          1,
          1 },
        { "unaligned",
          VK_FORMAT_R32_SFLOAT,
          1033,
          777,
          { 1, 2, 1031, 775 },
          VK_FORMAT_R32G32_SFLOAT,
          1033,
          777,
          { 1, 2, 1031, 775 },
          517,
          389,
          1,
          1 },
        { "doom_inflight",
          VK_FORMAT_D32_SFLOAT,
          2304,
          968,
          { 5, 3, 2293, 960 },
          VK_FORMAT_R32G32_SFLOAT,
          2293,
          960,
          { 0, 0, 2293, 960 },
          1720,
          720,
          20,
          3 },
    };

    if (fopen_s(&g_report, g_reportPath, "w") != 0 || g_report == nullptr)
        Fail("cannot write the report");
    std::fprintf(g_report, "{\"device\": \"%s\", \"cases\": [", g_gpu.deviceName.c_str());

    bool firstCase = true;
    for (const Case& c : cases)
    {
        Outcome outcome;
        const std::string note = RunCase(pass, c, outcome);
        Log("%-20s %8llu texels over %u frame(s), %llu depth and %llu motion mismatches%s%s", c.name,
            (unsigned long long) outcome.texels, c.frames, (unsigned long long) outcome.depthMismatches,
            (unsigned long long) outcome.motionMismatches, note.empty() ? "" : "; ", note.c_str());
        if (!outcome.first.empty())
            Log("  first mismatch: %s", outcome.first.c_str());
        std::fprintf(g_report,
                     "%s\n  {\"name\": \"%s\", \"out\": [%u, %u], \"frames\": %u, \"in_flight\": %u, \"texels\": %llu, "
                     "\"depth_mismatches\": %llu, \"motion_mismatches\": %llu, \"skipped\": %s}",
                     firstCase ? "" : ",", c.name, c.outW, c.outH, c.frames, c.inFlight,
                     (unsigned long long) outcome.texels, (unsigned long long) outcome.depthMismatches,
                     (unsigned long long) outcome.motionMismatches, note.empty() ? "false" : "true");
        firstCase = false;
    }

    // The way the route frees it: after the device has nothing left to run.
    Check(vkDeviceWaitIdle(g_gpu.device), "vkDeviceWaitIdle");
    pass.Destroy();
    if (pass.Ready())
        Fail("Destroy left the pass ready");

    vkDestroyCommandPool(g_gpu.device, g_gpu.pool, nullptr);
    vkDestroyDevice(g_gpu.device, nullptr);
    auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(g_gpu.instance, "vkDestroyDebugUtilsMessengerEXT"));
    const unsigned errors = g_validationErrors.load(); // after vkDestroyDevice, which reports leaked children
    destroyDebug(g_gpu.instance, g_gpu.messenger, nullptr);
    vkDestroyInstance(g_gpu.instance, nullptr);

    std::fprintf(g_report,
                 "\n], \"validation_active\": %s, \"sync_validation_active\": %s, \"validation_errors\": %u, "
                 "\"wine_injected_extension_messages\": %u}\n",
                 g_validationActive ? "true" : "false", g_syncActive ? "true" : "false", errors, g_wineInjected.load());
    std::fclose(g_report);
    Log("validation active, synchronization checks active, %u error(s)", errors);
    return 0;
}
