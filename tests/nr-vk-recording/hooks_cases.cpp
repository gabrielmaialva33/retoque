#include <dlssnr/DlssNr_VkRecording.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Fail notification-side allocation without changing the driver's result. Free must require no allocation.
std::atomic<bool> failAllocation = false;
void* operator new(std::size_t size)
{
    if (failAllocation.load())
        throw std::bad_alloc();
    if (void* allocation = std::malloc(size == 0 ? 1 : size))
        return allocation;
    throw std::bad_alloc();
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }

using VkDevice = struct Device*;
using VkCommandPool = struct Pool*;
using VkCommandBuffer = struct Buffer*;
using VkImage = struct Image*;
using VkResult = int;
using VkCommandBufferResetFlags = uint32_t;
using VkCommandPoolResetFlags = uint32_t;
struct VkAllocationCallbacks
{
};
struct VkCommandBufferBeginInfo
{
    int sType = 0;
    uint32_t flags = 0;
};
struct VkCommandBufferAllocateInfo
{
    int sType = 0;
    VkCommandPool commandPool = nullptr;
    int level = 0;
    uint32_t commandBufferCount = 0;
};
struct VkImageMemoryBarrier
{
};
constexpr auto VK_NULL_HANDLE = nullptr;
constexpr VkResult VK_SUCCESS = 0;
constexpr VkResult VK_ERROR_UNKNOWN = -13;
constexpr int VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 1;
constexpr int VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 2;
constexpr int VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0;
constexpr uint32_t VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 1;
constexpr int VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1;
constexpr int VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 2;
constexpr int VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 3;
constexpr int VK_IMAGE_LAYOUT_UNDEFINED = 4;
constexpr uint32_t VK_PIPELINE_STAGE_ALL_COMMANDS_BIT = 1;
constexpr uint32_t VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT = 2;

template <class... Args> void Log(Args&&...) {}
#define LOG_DEBUG(...) Log(__VA_ARGS__)
#define LOG_WARN(...) Log(__VA_ARGS__)
namespace magic_enum
{
template <class T> const char* enum_name(T) { return "result"; }
} // namespace magic_enum

using namespace DlssNr::VkRecording;
template <class T> T H(uintptr_t value) { return reinterpret_cast<T>(value); }
template <class T> Handle Key(T value) { return reinterpret_cast<Handle>(value); }
const VkDevice device = H<VkDevice>(1);
const VkCommandPool pool = H<VkCommandPool>(5);
Registry& R() { return Recordings(); }
void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct Driver
{
    VkResult allocateResult = VK_SUCCESS;
    VkResult beginResult = VK_SUCCESS;
    VkResult resetResult = VK_SUCCESS;
    VkResult resetPoolResult = VK_SUCCESS;
    VkResult endResult = VK_SUCCESS;
    VkResult virtualEndResult = VK_SUCCESS;
    uintptr_t nextBuffer = 100;
    VkCommandBuffer virtualBuffer = nullptr;
    VkCommandBuffer virtualTarget = nullptr;
    VkCommandBuffer lastFreed = nullptr;
    VkCommandBuffer lastBegin = nullptr;
    VkCommandPool lastPool = nullptr;
    VkDevice lastDevice = nullptr;
    int allocated = 0;
    int began = 0;
    int reset = 0;
    int freed = 0;
    int resetPools = 0;
    int destroyedPools = 0;
    int ended = 0;
    std::array<VkCommandBuffer, 4> endOrder {};
    std::weak_ptr<int> probe;
    bool aliveAtForward = false;
    bool pinAtBarrier = false;
    bool barrierHadTicket = false;
    std::weak_ptr<int> barrierPin;
} driver;

struct BridgeTracker
{
    int began = 0, ended = 0, reset = 0, freed = 0, allocated = 0, destroyedPools = 0, resetPools = 0;
    void OnBegin(VkCommandBuffer, const VkCommandBufferBeginInfo*) { ++began; }
    void OnEnd(VkCommandBuffer) { ++ended; }
    void OnReset(VkCommandBuffer) { ++reset; }
    void OnFreeCommandBuffers(VkCommandPool, uint32_t, const VkCommandBuffer*) { ++freed; }
    void OnAllocateCommandBuffers(VkCommandPool, uint32_t, const VkCommandBuffer*, std::optional<uint32_t>, int)
    {
        ++allocated;
    }
    void OnDestroyPool(VkCommandPool) { ++destroyedPools; }
    void OnResetPool(VkCommandPool) { ++resetPools; }
} cmdBufferStateTracker;

std::mutex mutexCommandPoolToQueueFamilyMap;
std::unordered_map<VkCommandPool, uint32_t> commandPoolToQueueFamilyMap;
VkCommandBuffer GetVirtualCommandBuffer(VkCommandBuffer) { return driver.virtualBuffer; }
VkCommandBuffer RemoveVirtualCommandBuffer(VkCommandBuffer)
{
    auto result = driver.virtualBuffer;
    driver.virtualBuffer = nullptr;
    return result;
}

VkResult DriverAllocate(VkDevice d, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers)
{
    ++driver.allocated;
    driver.lastDevice = d;
    driver.lastPool = info->commandPool;
    if (driver.allocateResult == VK_SUCCESS)
        for (uint32_t i = 0; i < info->commandBufferCount; ++i)
            buffers[i] = H<VkCommandBuffer>(driver.nextBuffer++);
    return driver.allocateResult;
}
VkResult DriverBegin(VkCommandBuffer buffer, const VkCommandBufferBeginInfo*)
{
    ++driver.began;
    driver.lastBegin = buffer;
    driver.aliveAtForward = !driver.probe.expired();
    return driver.beginResult;
}
VkResult DriverEnd(VkCommandBuffer buffer)
{
    if (driver.ended < static_cast<int>(driver.endOrder.size()))
        driver.endOrder[driver.ended] = buffer;
    ++driver.ended;
    return buffer == driver.virtualTarget ? driver.virtualEndResult : driver.endResult;
}
VkResult DriverReset(VkCommandBuffer, VkCommandBufferResetFlags)
{
    ++driver.reset;
    driver.aliveAtForward = !driver.probe.expired();
    return driver.resetResult;
}
void DriverFree(VkDevice d, VkCommandPool p, uint32_t count, const VkCommandBuffer* buffers)
{
    ++driver.freed;
    driver.lastDevice = d;
    driver.lastPool = p;
    driver.lastFreed = count ? buffers[0] : nullptr;
    driver.aliveAtForward = !driver.probe.expired();
}
VkResult DriverResetPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags)
{
    ++driver.resetPools;
    driver.aliveAtForward = !driver.probe.expired();
    return driver.resetPoolResult;
}
void DriverDestroyPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*)
{
    ++driver.destroyedPools;
    driver.aliveAtForward = !driver.probe.expired();
}
template <class... Args> VkImageMemoryBarrier Barrier(Args...) { return {}; }
void DriverBarrier(VkCommandBuffer buffer, uint32_t, uint32_t, uint32_t, uint32_t, const void*, uint32_t, const void*,
                   uint32_t, const VkImageMemoryBarrier*)
{
    if (!driver.pinAtBarrier)
        return;
    const auto ticket = R().Current(Key(device), Key(buffer));
    driver.barrierHadTicket = ticket.has_value();
    if (ticket)
    {
        auto token = std::make_shared<int>(1);
        driver.barrierPin = token;
        Check(R().Pin(*ticket, { token }), "barrier recording must retain its token");
    }
}

auto o_vkAllocateCommandBuffers = DriverAllocate;
auto o_vkBeginCommandBuffer = DriverBegin;
auto o_vkEndCommandBuffer = DriverEnd;
auto o_vkResetCommandBuffer = DriverReset;
auto o_vkFreeCommandBuffers = DriverFree;
auto o_vkResetCommandPool = DriverResetPool;
auto o_vkDestroyCommandPool = DriverDestroyPool;

struct Vulkan_wDx12
{
    static VkResult hk_vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
    static VkResult hk_vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*);
    static VkResult hk_vkEndCommandBuffer(VkCommandBuffer);
    static VkResult hk_vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags);
    static void hk_vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*);
    static VkResult hk_vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags);
    static void hk_vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*);
};

struct DeviceTransport
{
    VkDevice device = ::device;
    struct Functions
    {
        decltype(&DriverAllocate) allocateCommandBuffers = DriverAllocate;
        decltype(&DriverBegin) beginCommandBuffer = DriverBegin;
        decltype(&DriverEnd) endCommandBuffer = DriverEnd;
        decltype(&DriverReset) resetCommandBuffer = DriverReset;
        decltype(&DriverFree) freeCommandBuffers = DriverFree;
        decltype(&DriverBarrier) cmdPipelineBarrier = DriverBarrier;
    } fn;
    std::unordered_map<uint32_t, VkCommandPool> pools { { 3, pool } };
};
VkCommandPool PoolFor(DeviceTransport& dev, uint32_t family)
{
    auto it = dev.pools.find(family);
    return it == dev.pools.end() ? nullptr : it->second;
}
enum class Recorded
{
    Ok,
    Failed
};

// @production@

Ticket DirectRecording(VkCommandBuffer buffer)
{
    R().Allocate(Key(device), Key(pool), { Key(buffer) });
    R().Begin(Key(buffer));
    return *R().Current(Key(device), Key(buffer));
}
std::weak_ptr<int> Pin(const Ticket& ticket)
{
    auto token = std::make_shared<int>(1);
    Check(R().Pin(ticket, { token }), "fixture must pin a known recording");
    return token;
}
void Fresh()
{
    R().ForgetDevice(Key(device));
    driver = Driver {};
    cmdBufferStateTracker = BridgeTracker {};
    commandPoolToQueueFamilyMap.clear();
    commandPoolToQueueFamilyMap[pool] = 3;
}

void GameAllocationAndBegin()
{
    VkCommandBufferAllocateInfo info {};
    info.commandPool = pool;
    info.commandBufferCount = 2;
    std::array<VkCommandBuffer, 2> buffers {};
    Check(Vulkan_wDx12::hk_vkAllocateCommandBuffers(device, &info, buffers.data()) == VK_SUCCESS,
          "wrapper must preserve successful allocation result");
    Check(driver.allocated == 1 && cmdBufferStateTracker.allocated == 1, "original allocation/tracker must run once");
    VkCommandBufferBeginInfo begin {};
    for (auto buffer : buffers)
    {
        Check(!R().Current(Key(device), Key(buffer)), "allocated buffer must not start recording implicitly");
        Check(Vulkan_wDx12::hk_vkBeginCommandBuffer(buffer, &begin) == VK_SUCCESS, "Begin result must pass through");
        Check(R().Current(Key(device), Key(buffer)).has_value(), "actual returned allocation must be registered");
    }
    driver.allocateResult = VK_ERROR_UNKNOWN;
    auto unknown = H<VkCommandBuffer>(999);
    info.commandBufferCount = 1;
    Check(Vulkan_wDx12::hk_vkAllocateCommandBuffers(device, &info, &unknown) == VK_ERROR_UNKNOWN,
          "failed allocation result must pass through");
    Vulkan_wDx12::hk_vkBeginCommandBuffer(unknown, &begin);
    Check(!R().Current(Key(device), Key(unknown)), "failed allocation must not register its output");
}

void GameBeginOnlyAfterSuccess()
{
    auto buffer = H<VkCommandBuffer>(11);
    const auto old = DirectRecording(buffer);
    auto resource = Pin(old);
    driver.probe = resource;
    driver.beginResult = VK_ERROR_UNKNOWN;
    VkCommandBufferBeginInfo begin {};
    Check(Vulkan_wDx12::hk_vkBeginCommandBuffer(buffer, &begin) == VK_ERROR_UNKNOWN, "failed Begin result must remain");
    Check(!resource.expired() && R().Current(Key(device), Key(buffer))->epoch == old.epoch,
          "failed Begin must preserve the old recording and pins");
    driver.beginResult = VK_SUCCESS;
    Check(Vulkan_wDx12::hk_vkBeginCommandBuffer(buffer, &begin) == VK_SUCCESS, "successful Begin must pass through");
    Check(driver.aliveAtForward, "Begin must retain the resource until after the real call");
    Check(resource.expired(), "successful Begin must retire previous pins");
    Check(R().Current(Key(device), Key(buffer))->epoch != old.epoch, "Begin must start a fresh epoch");
}

void GameEndAndVirtualBuffers()
{
    auto buffer = H<VkCommandBuffer>(11);
    auto resource = Pin(DirectRecording(buffer));
    Check(Vulkan_wDx12::hk_vkEndCommandBuffer(buffer) == VK_SUCCESS, "End result must pass through");
    Check(!R().Current(Key(device), Key(buffer)), "End must close recording admission");
    Check(!resource.expired(), "End must preserve resources for resubmission");

    auto virtualBuffer = H<VkCommandBuffer>(12);
    DirectRecording(buffer);
    auto virtualResource = Pin(DirectRecording(virtualBuffer));
    driver.virtualBuffer = driver.virtualTarget = virtualBuffer;
    driver.endResult = VK_ERROR_UNKNOWN;
    driver.virtualEndResult = VK_SUCCESS;
    driver.ended = 0;
    const int legacyEnded = cmdBufferStateTracker.ended;
    Check(Vulkan_wDx12::hk_vkEndCommandBuffer(buffer) == VK_ERROR_UNKNOWN, "original End controls wrapper return");
    Check(driver.ended == 2 && driver.endOrder[0] == virtualBuffer && driver.endOrder[1] == buffer,
          "virtual End must preserve original call order");
    Check(cmdBufferStateTracker.ended == legacyEnded, "virtual End must preserve the legacy tracker branch");
    Check(!R().Current(Key(device), Key(virtualBuffer)) && !virtualResource.expired(),
          "successful virtual End must close only its recording without retiring pins");
    Check(R().Current(Key(device), Key(buffer)).has_value(), "failed original End must not close original recording");
}

void GameResetFreeAndPool()
{
    auto buffer = H<VkCommandBuffer>(11);
    auto resource = Pin(DirectRecording(buffer));
    driver.probe = resource;
    driver.resetResult = VK_ERROR_UNKNOWN;
    Check(Vulkan_wDx12::hk_vkResetCommandBuffer(buffer, 0) == VK_ERROR_UNKNOWN, "reset failure result must remain");
    Check(!resource.expired(), "failed reset must preserve pins");
    driver.resetResult = VK_SUCCESS;
    Vulkan_wDx12::hk_vkResetCommandBuffer(buffer, 0);
    Check(driver.aliveAtForward && resource.expired(), "reset must retire only after real success");

    resource = Pin(DirectRecording(buffer));
    driver.probe = resource;
    Vulkan_wDx12::hk_vkFreeCommandBuffers(device, pool, 1, &buffer);
    Check(driver.freed == 1 && driver.lastFreed == buffer && driver.lastDevice == device && driver.lastPool == pool,
          "Free must forward the actual device, pool and buffer once");
    Check(driver.aliveAtForward && resource.expired(), "Free must retire only after forwarding");
    R().Begin(Key(buffer));
    Check(!R().Current(Key(device), Key(buffer)), "Free must remove the allocation record");

    resource = Pin(DirectRecording(buffer));
    auto other = Pin(DirectRecording(H<VkCommandBuffer>(12)));
    driver.probe = resource;
    driver.resetPoolResult = VK_ERROR_UNKNOWN;
    Vulkan_wDx12::hk_vkResetCommandPool(device, pool, 0);
    Check(!resource.expired() && !other.expired(), "failed pool reset must preserve every pin");
    driver.resetPoolResult = VK_SUCCESS;
    Vulkan_wDx12::hk_vkResetCommandPool(device, pool, 0);
    Check(driver.aliveAtForward && resource.expired() && other.expired(), "successful pool reset must retire its pins");

    resource = Pin(DirectRecording(buffer));
    driver.probe = resource;
    Vulkan_wDx12::hk_vkDestroyCommandPool(device, pool, nullptr);
    Check(driver.destroyedPools == 1 && driver.aliveAtForward && resource.expired(),
          "pool destruction must forward before retiring its records");
    R().Begin(Key(buffer));
    Check(!R().Current(Key(device), Key(buffer)), "destroyed pool must remove allocation records");
}

void HostBypassLifecycle()
{
    DeviceTransport dev;
    auto buffer = AllocateCommandBuffer(dev, 3);
    Check(buffer != nullptr && Begin(dev, buffer), "host must preserve allocation and Begin results");
    const auto ticket = R().Current(Key(device), Key(buffer));
    Check(ticket.has_value(), "host trampoline bypass must explicitly register and begin");
    auto resource = Pin(*ticket);
    driver.probe = resource;
    driver.resetResult = VK_ERROR_UNKNOWN;
    Check(!Begin(dev, buffer) && !resource.expired(), "host reset failure must preserve pins");
    driver.resetResult = VK_SUCCESS;
    driver.beginResult = VK_ERROR_UNKNOWN;
    Check(!Begin(dev, buffer), "host Begin failure must pass through");
    Check(resource.expired() && !R().Current(Key(device), Key(buffer)),
          "successful host reset must retire even when subsequent Begin fails");
    driver.beginResult = VK_SUCCESS;
    Check(Begin(dev, buffer), "host recording must recover on a later successful Begin");
    resource = Pin(*R().Current(Key(device), Key(buffer)));
    driver.probe = resource;
    const auto original = buffer;
    FreeCommandBuffer(dev, 3, buffer);
    Check(buffer == nullptr && driver.lastFreed == original && driver.aliveAtForward && resource.expired(),
          "host Free must retain until forwarding, retire and clear the caller's handle");
}

void HostRecordingEndSites()
{
    DeviceTransport dev;
    auto buffer = AllocateCommandBuffer(dev, 3);
    driver.pinAtBarrier = true;
    Check(RecordHandOver(dev, buffer, H<VkImage>(9), true, 3, 4), "hand-over recording must complete");
    Check(driver.barrierHadTicket && !R().Current(Key(device), Key(buffer)) && !driver.barrierPin.expired(),
          "hand-over must have open recording during commands and retain it after End");
    Check(RecordRecovery(dev, buffer, H<VkImage>(9)), "recovery recording must complete");
    Check(driver.barrierHadTicket && !R().Current(Key(device), Key(buffer)) && !driver.barrierPin.expired(),
          "recovery must close admission while preserving recorded pins");
    Check(Begin(dev, buffer), "work-tail fixture must begin");
    auto resource = Pin(*R().Current(Key(device), Key(buffer)));
    Check(EndRecordedTail(dev, buffer) == Recorded::Ok, "work recording must preserve its result enum");
    Check(!R().Current(Key(device), Key(buffer)) && !resource.expired(),
          "work recording's actual End tail must close admission without retiring pins");
}

void DoubleNotificationsAreHarmless()
{
    DeviceTransport dev;
    dev.fn.allocateCommandBuffers = Vulkan_wDx12::hk_vkAllocateCommandBuffers;
    dev.fn.beginCommandBuffer = Vulkan_wDx12::hk_vkBeginCommandBuffer;
    dev.fn.resetCommandBuffer = Vulkan_wDx12::hk_vkResetCommandBuffer;
    dev.fn.endCommandBuffer = Vulkan_wDx12::hk_vkEndCommandBuffer;
    dev.fn.freeCommandBuffers = Vulkan_wDx12::hk_vkFreeCommandBuffers;
    auto buffer = AllocateCommandBuffer(dev, 3);
    Check(Begin(dev, buffer), "host through wrapper must begin");
    auto ticket = R().Current(Key(device), Key(buffer));
    Check(ticket.has_value(), "double lifecycle notifications must leave an open recording");
    auto resource = Pin(*ticket);
    Check(EndRecordedTail(dev, buffer) == Recorded::Ok, "double End must preserve result");
    Check(!resource.expired(), "double End must preserve pins inserted after Begin");
    driver.probe = resource;
    FreeCommandBuffer(dev, 3, buffer);
    Check(driver.freed == 1 && driver.aliveAtForward && resource.expired(),
          "double Free notifications must call the driver once and release the recording");
}

void AllocationFailureDoesNotCrossBoundary()
{
    VkCommandBufferAllocateInfo info {};
    info.commandPool = pool;
    info.commandBufferCount = 1;
    VkCommandBuffer buffer = nullptr;
    VkResult result = VK_ERROR_UNKNOWN;
    bool threw = false;
    failAllocation.store(true);
    try
    {
        result = Vulkan_wDx12::hk_vkAllocateCommandBuffers(device, &info, &buffer);
    }
    catch (const std::bad_alloc&)
    {
        threw = true;
    }
    failAllocation.store(false);
    Check(!threw && result == VK_SUCCESS && driver.allocated == 1 && buffer != nullptr,
          "new tracking allocation failure must not escape the Vulkan boundary or change driver success");
    R().Begin(Key(buffer));
    Check(!R().Current(Key(device), Key(buffer)), "failed registry adapter allocation must stay inadmissible");

    auto resource = Pin(DirectRecording(buffer));
    driver.probe = resource;
    failAllocation.store(true);
    try
    {
        Vulkan_wDx12::hk_vkFreeCommandBuffers(device, pool, 1, &buffer);
    }
    catch (const std::bad_alloc&)
    {
        threw = true;
    }
    failAllocation.store(false);
    Check(!threw && driver.freed == 1 && driver.aliveAtForward && resource.expired(),
          "Free must retire after forwarding even when every new allocation fails");
    R().Begin(Key(buffer));
    Check(!R().Current(Key(device), Key(buffer)), "allocation-free Free must remove the allocation record");
}

void HostFreeNeedsNoAllocation()
{
    DeviceTransport dev;
    auto buffer = AllocateCommandBuffer(dev, 3);
    Check(Begin(dev, buffer), "host allocation fixture must begin");
    auto resource = Pin(*R().Current(Key(device), Key(buffer)));
    driver.probe = resource;
    const auto original = buffer;
    bool threw = false;
    failAllocation.store(true);
    try
    {
        FreeCommandBuffer(dev, 3, buffer);
    }
    catch (const std::bad_alloc&)
    {
        threw = true;
    }
    failAllocation.store(false);
    Check(!threw && driver.freed == 1 && driver.lastFreed == original && driver.aliveAtForward,
          "host Free must forward once before retirement without allocating");
    Check(buffer == nullptr && resource.expired(), "host Free under OOM must clear the handle and retire its pins");
    R().Begin(Key(original));
    Check(!R().Current(Key(device), Key(original)), "host Free under OOM must remove the allocation record");
}

void ReusedHandleAfterOutOfMemory(bool host)
{
    DeviceTransport dev;
    const auto newPool = H<VkCommandPool>(6);
    dev.pools.emplace(4, newPool);
    commandPoolToQueueFamilyMap[newPool] = 4;
    auto buffer = H<VkCommandBuffer>(100);
    auto old = DirectRecording(buffer);
    auto resource = Pin(old);
    driver.probe = resource;
    VkCommandBufferAllocateInfo info {};
    info.commandPool = newPool;
    info.commandBufferCount = 1;
    const auto recycled = buffer;
    bool threw = false;
    failAllocation.store(true);
    try
    {
        if (host)
            FreeCommandBuffer(dev, 3, buffer);
        else
            Vulkan_wDx12::hk_vkFreeCommandBuffers(device, pool, 1, &buffer);
        driver.nextBuffer = Key(recycled);
        if (host)
            buffer = AllocateCommandBuffer(dev, 4);
        else
            Vulkan_wDx12::hk_vkAllocateCommandBuffers(device, &info, &buffer);
    }
    catch (const std::bad_alloc&)
    {
        threw = true;
    }
    failAllocation.store(false);
    Check(!threw && buffer == recycled && driver.lastPool == newPool,
          "native handle reuse in a different pool must preserve driver success under OOM");
    VkCommandBufferBeginInfo begin {};
    if (host)
        Check(Begin(dev, buffer), "host must preserve successful Begin on an untracked allocation");
    else
        Check(Vulkan_wDx12::hk_vkBeginCommandBuffer(buffer, &begin) == VK_SUCCESS,
              "wrapper must preserve successful Begin on an untracked allocation");
    const auto stale = R().Current(Key(device), Key(buffer));
    if (stale)
    {
        auto incorrectlyAdmitted = Pin(*stale);
        Vulkan_wDx12::hk_vkDestroyCommandPool(device, pool, nullptr);
        Check(!incorrectlyAdmitted.expired(),
              "destroying the old pool must never retire a recording belonging to the new native pool");
    }
    Check(!stale, "an allocation notification lost under OOM must not inherit the retired pool's record");
    Check(resource.expired(), "the old allocation's pins must retire despite OOM");

    // Once tracking can allocate again, a new successful allocation is admitted under the actual new pool.
    if (host)
        FreeCommandBuffer(dev, 4, buffer);
    else
        Vulkan_wDx12::hk_vkFreeCommandBuffers(device, newPool, 1, &buffer);
    driver.nextBuffer = Key(recycled);
    if (host)
        buffer = AllocateCommandBuffer(dev, 4);
    else
        Vulkan_wDx12::hk_vkAllocateCommandBuffers(device, &info, &buffer);
    if (host)
        Check(Begin(dev, buffer), "host tracking must recover after OOM");
    else
        Vulkan_wDx12::hk_vkBeginCommandBuffer(buffer, &begin);
    const auto current = R().Current(Key(device), Key(buffer));
    Check(current.has_value(), "successful later allocation must recover admission");
    auto recovered = Pin(*current);
    Check(!R().Pin(old, { std::make_shared<int>(1) }), "recycled handle must still reject the old recording ticket");
    Vulkan_wDx12::hk_vkDestroyCommandPool(device, pool, nullptr);
    Check(!recovered.expired(), "old pool retirement must preserve correctly registered new-pool recording");
    Vulkan_wDx12::hk_vkDestroyCommandPool(device, newPool, nullptr);
    Check(recovered.expired(), "new pool retirement must release its recording");
}

void GameHandleReuseAfterOutOfMemory() { ReusedHandleAfterOutOfMemory(false); }
void HostHandleReuseAfterOutOfMemory() { ReusedHandleAfterOutOfMemory(true); }

void GameBulkFreePreservesReentrantAllocation()
{
    const std::array<VkCommandBuffer, 2> buffers { H<VkCommandBuffer>(11), H<VkCommandBuffer>(12) };
    const auto firstTicket = DirectRecording(buffers[0]);
    auto oldSecond = Pin(DirectRecording(buffers[1]));
    bool reentered = false;
    bool allDetached = false;
    std::weak_ptr<int> replacement;
    auto first = std::shared_ptr<int>(nullptr,
                                      [&](int*)
                                      {
                                          allDetached = !R().Current(Key(device), Key(buffers[0])) &&
                                                        !R().Current(Key(device), Key(buffers[1]));
                                          VkCommandBufferAllocateInfo info {};
                                          info.commandPool = pool;
                                          info.commandBufferCount = 1;
                                          driver.nextBuffer = Key(buffers[1]);
                                          VkCommandBuffer reused = nullptr;
                                          Vulkan_wDx12::hk_vkAllocateCommandBuffers(device, &info, &reused);
                                          VkCommandBufferBeginInfo begin {};
                                          Vulkan_wDx12::hk_vkBeginCommandBuffer(reused, &begin);
                                          const auto ticket = R().Current(Key(device), Key(reused));
                                          auto token = std::make_shared<int>(3);
                                          replacement = token;
                                          reentered = reused == buffers[1] && ticket && R().Pin(*ticket, { token });
                                      });
    Check(R().Pin(firstTicket, { first }), "setup must pin the reentrant first member");
    first.reset();
    Vulkan_wDx12::hk_vkFreeCommandBuffers(device, pool, buffers.size(), buffers.data());
    Check(reentered && !replacement.expired() && R().Current(Key(device), Key(buffers[1])),
          "bulk wDx12 Free must preserve a handle recycled by its first release callback");
    Check(allDetached && oldSecond.expired() && driver.freed == 1,
          "bulk wDx12 Free must forward once and remove all old metadata before callbacks");
    Vulkan_wDx12::hk_vkResetCommandBuffer(buffers[1], 0);
    Check(replacement.expired(), "reentrant allocation must survive until its own later reset");
}

int main()
{
    const std::pair<const char*, void (*)()> cases[] = {
        { "game-allocation-begin", GameAllocationAndBegin },
        { "game-begin-success-boundary", GameBeginOnlyAfterSuccess },
        { "game-end-virtual", GameEndAndVirtualBuffers },
        { "game-reset-free-pool", GameResetFreeAndPool },
        { "host-bypass-lifecycle", HostBypassLifecycle },
        { "host-end-sites", HostRecordingEndSites },
        { "double-notifications", DoubleNotificationsAreHarmless },
        { "allocation-failure-boundary", AllocationFailureDoesNotCrossBoundary },
        { "host-free-without-allocation", HostFreeNeedsNoAllocation },
        { "game-handle-reuse-after-oom", GameHandleReuseAfterOutOfMemory },
        { "host-handle-reuse-after-oom", HostHandleReuseAfterOutOfMemory },
        { "game-bulk-free-reentrant-reuse", GameBulkFreePreservesReentrantAllocation },
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
            failAllocation.store(false);
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    R().ForgetDevice(Key(device));
    return failures == 0 ? 0 : 1;
}
