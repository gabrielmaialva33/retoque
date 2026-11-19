#include "pch.h"
#include "DlssNr_VkChain.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cassert>
#include <iostream>

// Execute production queue wrappers with a driver boundary fake. This characterizes the
// missing adapter; a passing test does NOT mean submission/completion is implemented.
using namespace DlssNr;
template <class T> uint64_t H(T value) { return (uint64_t) value; }
template <class T> T Handle(uint64_t value) { return (T) value; }
struct QueueMutex
{
    unsigned held = 0;
    void lock() { ++held; }
    void unlock() { --held; }
    unsigned Owner() const { return held; }
};
struct QueueLocks
{
    QueueMutex submit;
};
std::array<QueueLocks, 2> queueLocks;
QueueLocks* QueueLocksFor(VkQueue queue)
{
    return H(queue) >= 1 && H(queue) <= 2 ? &queueLocks[H(queue) - 1] : nullptr;
}
constexpr int kLockSubmit = 1;
struct HostActivity
{
    HostActivity(const char*, uint64_t) {}
    struct Scope
    {
        void Stage(const char*, int = 0, unsigned = 0) {}
        void Hold(int, bool) {}
    } scope;
};
VkResult driverResult = VK_SUCCESS;
VkQueue expectedQueue = VK_NULL_HANDLE;
VkFence expectedFence = VK_NULL_HANDLE;
const void* expectedSubmits = nullptr;
unsigned calls = 0;

template <class Info> VkResult Forward(VkQueue queue, uint32_t count, const Info* info, VkFence fence)
{
    assert(queue == expectedQueue && fence == expectedFence && count == 1 && info == expectedSubmits);
    if (auto* locks = QueueLocksFor(queue))
        assert(locks->submit.held == 1);
    // Reenter the REAL recording registry from the fake driver: queue callbacks cannot hold its mutex.
    assert(!VkRecording::Recordings().Current(9, 7)); // executable, not open
    ++calls;
    return driverResult;
}
VkResult VKAPI_CALL Submit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) { return Forward(q, n, s, f); }
VkResult VKAPI_CALL Submit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) { return Forward(q, n, s, f); }
template <class Pfn> struct Targets
{
    std::array<Pfn, 4> next {};
};
Targets<PFN_vkQueueSubmit> g_submit { { Submit, Submit } };
Targets<PFN_vkQueueSubmit2> g_submit2 { { Submit2, Submit2 } };

// @production@

int main()
{
    auto& registry = VkRecording::Recordings();
    registry.Allocate(9, 8, { 7 }, VkRecording::Level::Primary);
    registry.Begin(7, VkRecording::Reuse::Serial);
    const auto ticket = *registry.Current(9, 7);
    auto pin = std::make_shared<int>(0);
    assert(registry.Pin(ticket, { pin }));
    registry.End(7);
    VkChain::Creation creation(ticket, 1);
    VkCommandBuffer buffer = Handle<VkCommandBuffer>(7);
    VkSubmitInfo legacy {};
    legacy.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    legacy.commandBufferCount = 1;
    legacy.pCommandBuffers = &buffer;
    VkCommandBufferSubmitInfo command {};
    command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command.commandBuffer = buffer;
    VkSubmitInfo2 modern {};
    modern.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    modern.commandBufferInfoCount = 1;
    modern.pCommandBufferInfos = &command;
    for (uint64_t queue : { 1, 2, 3 }) // two known queues and an unobserved queue
        for (auto result : { VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_DEVICE_LOST })
            for (unsigned api = 0; api < 3; ++api)
            {
                driverResult = result;
                expectedQueue = Handle<VkQueue>(queue);
                expectedFence = Handle<VkFence>(10); // borrowed/recycled application fence, never proof
                expectedSubmits = api == 0 ? static_cast<void*>(&legacy) : static_cast<void*>(&modern);
                const auto before = calls;
                const auto observed = api == 0   ? hkQueueSubmit<0>(expectedQueue, 1, &legacy, expectedFence)
                                      : api == 1 ? hkQueueSubmit2<0>(expectedQueue, 1, &modern, expectedFence)
                                                 : hkQueueSubmit2<1>(expectedQueue, 1, &modern, expectedFence);
                assert(observed == result && calls == before + 1);
                assert(queueLocks[0].submit.held == 0 && queueLocks[1].submit.held == 0);
                assert(pin.use_count() == 2); // none of these outcomes retires an executable recording
                assert(!creation.Ready(100)); // neither success nor a fence handle feeds the creation proof
            }
    registry.Reset(7);
    assert(pin.use_count() == 1 && !creation.Ready(100));
    registry.ForgetDevice(9);
    std::cout << "PASS: 27 production submit forwarding cases; no creation completion adapter claimed\n";
}
