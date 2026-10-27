// Only the Vulkan allocation API boundary is substituted. Production functions
// below are extracted byte-for-byte by run.py; no GPU execution is claimed.
#include "fake_vulkan.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define LOG_WARN(...) ((void) 0)
constexpr uint32_t kMeterSide = 8;
constexpr VkDeviceSize kMeterBytes = kMeterSide * kMeterSide * sizeof(float);
constexpr unsigned long long kMeterSlots = 4;
struct State
{
    VkDevice device = reinterpret_cast<VkDevice>(uintptr_t { 1 });
    VkBuffer meterReadback[4] = {};
    VkDeviceMemory meterReadbackMemory[4] = {};
    void* meterMapped[4] = {};
    unsigned long long meterFrames = 0;
} g_vk;

enum class Stage
{
    None,
    Create,
    MemoryType,
    Allocate,
    Bind,
    Map
};
struct Buffer
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
};
struct Memory
{
    bool mapped = false;
};
std::map<VkBuffer, Buffer> buffers;
std::map<VkDeviceMemory, Memory> memories;
std::vector<std::string> violations;
Stage failStage = Stage::None;
unsigned failSlot = 0, currentSlot = 0, creates = 0;
uint64_t nextHandle = 10;
unsigned destroys = 0, frees = 0, unmaps = 0;
void Check(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}
void Require(bool condition, const std::string& message)
{
    if (!condition)
        violations.push_back(message);
}
bool Fail(Stage stage) { return stage == failStage && currentSlot == failSlot; }
VkResult vkCreateBuffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* result)
{
    currentSlot = creates++;
    Require(info->size == kMeterBytes && info->usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            "incorrect readback buffer contract");
    if (Fail(Stage::Create))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    *result = nextHandle++;
    buffers.emplace(*result, Buffer {});
    return VK_SUCCESS;
}
void vkGetBufferMemoryRequirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* requirements)
{
    Require(buffers.count(buffer) == 1, "requirements for unknown buffer");
    *requirements = { kMeterBytes + 256, 64, 1 };
}
uint32_t FindMemoryTypeIndex(uint32_t bits, VkMemoryPropertyFlags flags)
{
    Require(bits == 1 && flags == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
            "incorrect memory type requirements");
    return Fail(Stage::MemoryType) ? UINT32_MAX : 0;
}
VkResult vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*,
                          VkDeviceMemory* result)
{
    Require(info->allocationSize == kMeterBytes + 256 && info->memoryTypeIndex == 0,
            "incorrect memory allocation contract");
    if (Fail(Stage::Allocate))
    {
        *result = 0xdeadbeef; // A failed Vulkan call leaves output parameters unspecified.
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *result = nextHandle++;
    memories.emplace(*result, Memory {});
    return VK_SUCCESS;
}
VkResult vkBindBufferMemory(VkDevice, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
    Require(buffers.count(buffer) == 1 && memories.count(memory) == 1 && offset == 0, "invalid bind");
    if (Fail(Stage::Bind))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    buffers.at(buffer).memory = memory;
    return VK_SUCCESS;
}
VkResult vkMapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags,
                     void** mapped)
{
    Require(memories.count(memory) == 1 && offset == 0 && size == kMeterBytes && flags == 0, "invalid map");
    if (Fail(Stage::Map))
        return VK_ERROR_MEMORY_MAP_FAILED;
    memories.at(memory).mapped = true;
    *mapped = reinterpret_cast<void*>(static_cast<uintptr_t>(memory * 16));
    return VK_SUCCESS;
}
void vkUnmapMemory(VkDevice, VkDeviceMemory memory)
{
    Require(memories.count(memory) == 1 && memories.at(memory).mapped, "invalid or duplicate unmap");
    memories.at(memory).mapped = false;
    ++unmaps;
}
void vkDestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*)
{
    Require(buffers.erase(buffer) == 1, "invalid or duplicate buffer destroy");
    ++destroys;
}
void vkFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*)
{
    for (const auto& [handle, buffer] : buffers)
    {
        (void) handle;
        Require(buffer.memory != memory, "memory freed before bound buffer destruction");
    }
    Require(memories.count(memory) == 1 && !memories.at(memory).mapped, "invalid or mapped memory free");
    memories.erase(memory);
    ++frees;
}

// PRODUCTION_FUNCTIONS

using Tuple = std::array<uintptr_t, 3>;
Tuple Slot(unsigned i)
{
    return { static_cast<uintptr_t>(g_vk.meterReadback[i]), static_cast<uintptr_t>(g_vk.meterReadbackMemory[i]),
             reinterpret_cast<uintptr_t>(g_vk.meterMapped[i]) };
}
bool Empty(unsigned i) { return Slot(i) == Tuple {}; }
bool Complete(unsigned i) { return g_vk.meterReadback[i] && g_vk.meterReadbackMemory[i] && g_vk.meterMapped[i]; }
void VerifyOwnership(unsigned expected)
{
    Check(buffers.size() == expected && memories.size() == expected, "orphaned or leaked allocation");
    for (unsigned i = 0; i < 4; ++i)
    {
        Check(Empty(i) || Complete(i), "incomplete tuple published");
        if (Complete(i))
        {
            Check(buffers.count(g_vk.meterReadback[i]) == 1, "published dead buffer");
            Check(memories.count(g_vk.meterReadbackMemory[i]) == 1, "published dead memory");
            Check(buffers.at(g_vk.meterReadback[i]).memory == g_vk.meterReadbackMemory[i], "wrong bound memory");
            Check(memories.at(g_vk.meterReadbackMemory[i]).mapped, "published unmapped memory");
        }
    }
    Check(violations.empty(), violations.empty() ? "" : violations.front());
}
void Reset()
{
    g_vk = State {};
    buffers.clear();
    memories.clear();
    violations.clear();
    failStage = Stage::None;
    failSlot = currentSlot = creates = 0;
    destroys = frees = unmaps = 0;
    nextHandle = 10;
}
void FailureCase(Stage stage, unsigned slot)
{
    Reset();
    failStage = stage;
    failSlot = slot;
    Check(!CreateMeterReadback(), "injected failure must fail allocation");
    for (unsigned i = 0; i < 4; ++i)
        Check(i < slot ? Complete(i) : Empty(i), "failure must retain only previous complete slots");
    VerifyOwnership(slot);
    std::array<Tuple, 4> before {};
    for (unsigned i = 0; i < slot; ++i)
        before[i] = Slot(i);
    failStage = Stage::None;
    Check(CreateMeterReadback(), "retry did not complete ring");
    for (unsigned i = 0; i < 4; ++i)
    {
        Check(Complete(i), "retry skipped an incomplete slot");
        if (i < slot)
            Check(Slot(i) == before[i], "retry modified a completed slot");
    }
    VerifyOwnership(4);
    const unsigned previousCreates = creates;
    Check(CreateMeterReadback() && creates == previousCreates, "complete ring allocation was not idempotent");
    g_vk.meterFrames = 77;
    DestroyMeterReadback();
    VerifyOwnership(0);
    Check(g_vk.meterFrames == 0, "destroy did not reset meter frame count");
    for (unsigned i = 0; i < 4; ++i)
        Check(Empty(i), "destroy did not clear slot");
    const auto counts = std::array { destroys, frees, unmaps };
    DestroyMeterReadback();
    Check(counts == std::array { destroys, frees, unmaps }, "destroy released an allocation twice");
    VerifyOwnership(0);
}
void DestructionCase()
{
    Reset();
    Check(CreateMeterReadback(), "full allocation failed");
    DestroyMeterReadback();
    VerifyOwnership(0);
    Check(destroys == 4 && frees == 4 && unmaps == 4, "full ring did not release each resource once");
    DestroyMeterReadback();
    Check(destroys == 4 && frees == 4 && unmaps == 4, "repeat destroy was not inert");
}
void MixedCleanupCase()
{
    Reset();
    buffers.emplace(21, Buffer { 22 });
    memories.emplace(22, Memory { true });
    g_vk.meterReadback[0] = 21;
    g_vk.meterReadbackMemory[0] = 22;
    g_vk.meterMapped[0] = reinterpret_cast<void*>(uintptr_t { 23 });
    buffers.emplace(31, Buffer {});
    g_vk.meterReadback[1] = 31;
    memories.emplace(42, Memory { true });
    g_vk.meterReadbackMemory[2] = 42;
    g_vk.meterMapped[2] = reinterpret_cast<void*>(uintptr_t { 43 });
    buffers.emplace(51, Buffer { 52 });
    memories.emplace(52, Memory { false });
    g_vk.meterReadback[3] = 51;
    g_vk.meterReadbackMemory[3] = 52;
    DestroyMeterReadback();
    VerifyOwnership(0);
    for (unsigned i = 0; i < 4; ++i)
        Check(Empty(i), "mixed cleanup did not clear slot");
    Check(destroys == 3 && frees == 3 && unmaps == 2, "mixed cleanup released incorrect resources");
    DestroyMeterReadback();
    Check(destroys == 3 && frees == 3 && unmaps == 2, "mixed cleanup released resources twice");
}
void ReadinessCase()
{
#if TEST_HAS_READY
    Reset();
    for (unsigned mask = 0; mask < 8; ++mask)
    {
        g_vk.meterReadback[2] = (mask & 1) ? 101 : VK_NULL_HANDLE;
        g_vk.meterReadbackMemory[2] = (mask & 2) ? 102 : VK_NULL_HANDLE;
        g_vk.meterMapped[2] = (mask & 4) ? reinterpret_cast<void*>(uintptr_t { 103 }) : nullptr;
        Check(MeterReadbackReady(2) == (mask == 7), "readiness accepted an incomplete tuple");
    }
    Reset();
#else
    throw std::runtime_error("production complete-tuple readiness helper is missing");
#endif
}
int main()
{
    unsigned failed = 0;
    for (Stage stage : { Stage::Create, Stage::MemoryType, Stage::Allocate, Stage::Bind, Stage::Map })
        for (unsigned slot = 0; slot < 4; ++slot)
        {
            try
            {
                FailureCase(stage, slot);
            }
            catch (const std::exception& error)
            {
                ++failed;
                std::cerr << "FAIL stage=" << static_cast<unsigned>(stage) << " slot=" << slot << ": " << error.what()
                          << '\n';
            }
        }
    for (auto test : { DestructionCase, MixedCleanupCase, ReadinessCase })
        try
        {
            test();
        }
        catch (const std::exception& error)
        {
            ++failed;
            std::cerr << "FAIL: " << error.what() << '\n';
        }
    if (failed)
    {
        std::cerr << failed << " meter cases failed\n";
        return 1;
    }
    std::cout
        << "PASS: 20 per-slot allocation failure/retry cases, readiness masks and exact-once cleanup (host only)\n";
}
