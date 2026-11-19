#include "pch.h"
#include "DlssNr_VkCreation.h"
#include <cassert>
#include <iostream>
#include <map>

using namespace DlssNr;
template <class T> T H(uint64_t value) { return (T) value; }
struct CountingMutex
{
    inline static unsigned locks = 0;
    bool held = false;
    void lock()
    {
        assert(!held);
        held = true;
        ++locks;
    }
    void unlock()
    {
        assert(held);
        held = false;
    }
};
using Observer = VkChain::CreationObserver<CountingMutex>;
struct Fence
{
    VkQueue queue = VK_NULL_HANDLE;
    bool signalled = false;
};
std::map<VkFence, Fence> fences;
uint64_t nextFence = 10;
unsigned creates = 0, markers = 0, polls = 0, destroys = 0;
VkResult createResult = VK_SUCCESS, markerResult = VK_SUCCESS, pollResult = VK_NOT_READY;
bool drained = false, driverLost = false;
VkResult VKAPI_CALL Create(VkDevice, const VkFenceCreateInfo* info, const VkAllocationCallbacks*, VkFence* fence)
{
    assert(!driverLost && info->flags == 0);
    ++creates;
    if (createResult == VK_ERROR_DEVICE_LOST)
        driverLost = true;
    if (createResult == VK_SUCCESS)
    {
        *fence = H<VkFence>(nextFence++);
        fences[*fence] = {};
    }
    return createResult;
}
void VKAPI_CALL Destroy(VkDevice, VkFence fence, const VkAllocationCallbacks*)
{
    assert(!driverLost && (drained || fences.at(fence).signalled));
    ++destroys;
    fences.erase(fence);
}
VkResult VKAPI_CALL Status(VkDevice, VkFence fence)
{
    assert(!driverLost);
    ++polls;
    if (pollResult == VK_ERROR_DEVICE_LOST)
        driverLost = true;
    return fences.at(fence).signalled ? VK_SUCCESS : pollResult;
}
VkResult VKAPI_CALL Marker(VkQueue queue, uint32_t count, const VkSubmitInfo* submit, VkFence fence)
{
    assert(!driverLost && count == 1 && submit && submit->sType == VK_STRUCTURE_TYPE_SUBMIT_INFO);
    assert(!submit->pNext && !submit->commandBufferCount && !submit->waitSemaphoreCount &&
           !submit->signalSemaphoreCount);
    ++markers;
    fences.at(fence).queue = queue;
    if (markerResult == VK_ERROR_DEVICE_LOST)
        driverLost = true;
    return markerResult;
}
const VkChain::CompletionFunctions functions { Create, Destroy, Status, Marker };
const VkDevice device = H<VkDevice>(9);
VkRecording::Registry registry;
VkRecording::Ticket Begin(uint64_t buffer, VkRecording::Level level = VkRecording::Level::Primary,
                          VkRecording::Reuse reuse = VkRecording::Reuse::Serial)
{
    registry.Allocate(9, 8, { buffer }, level);
    registry.Begin(buffer, reuse, true);
    return *registry.Current(9, buffer);
}
VkResult Submit(Observer& observer, uint64_t queue, std::initializer_list<uint64_t> buffers,
                VkResult result = VK_SUCCESS)
{
    unsigned called = 0;
    auto answer = observer.Submit(
        H<VkQueue>(queue),
        [&](auto visit)
        {
            for (auto buffer : buffers)
                visit(H<VkCommandBuffer>(buffer));
        },
        [&]
        {
            ++called;
            registry.Current(9, 7);
            return result;
        });
    assert(answer == result && called == 1);
    return answer;
}
void Clean(Observer& observer)
{
    drained = true;
    observer.Destroy(!driverLost);
    registry.ForgetDevice(9);
    fences.clear(); // CPU fake only: lost-device handles deliberately were not destroyed by production
    creates = markers = polls = destroys = 0;
    createResult = markerResult = VK_SUCCESS;
    pollResult = VK_NOT_READY;
    driverLost = drained = false;
}
int main()
{
    Observer observer;
    const auto idleLocks = CountingMutex::locks;
    Submit(observer, 1, { 7 });
    assert(CountingMutex::locks == idleLocks && !creates && !markers && !polls);
    assert(observer.Initialize(device, functions));
    for (auto level : { VkRecording::Level::Secondary, VkRecording::Level::Unknown })
        assert(!observer.Watch(Begin(7, level), 1, registry));
    assert(!observer.Watch(Begin(7, VkRecording::Level::Primary, VkRecording::Reuse::Simultaneous), 1, registry));
    assert(!observer.Watch(Begin(7, VkRecording::Level::Primary, VkRecording::Reuse::Unknown), 1, registry));
    auto reusable = Begin(7);
    registry.Begin(7, VkRecording::Reuse::Serial, false);
    assert(!observer.Watch(*registry.Current(9, 7), 1, registry));
    (void) reusable;
    auto ticket = Begin(7);
    const auto serial = observer.Watch(ticket, 1, registry);
    assert(serial && !observer.Watch(ticket, 1, registry));
    registry.End(7);
    assert(!observer.Ready(serial, 999));
    Submit(observer, 2, { 8 });
    assert(!creates);
    Submit(observer, 1, { 7 });
    assert(creates == 1 && markers == 1 && fences.begin()->second.queue == H<VkQueue>(1));
    assert(!observer.Ready(serial, 999));
    fences.begin()->second.signalled = true;
    assert(!observer.Ready(serial, 1));
    assert(observer.Ready(serial, 2) && destroys == 1);
    registry.Reset(7);
    assert(!observer.Ready(serial, 3));
    Clean(observer);

    // Native handle reuse may choose a DIFFERENT observer slot; serials must still not collide.
    Observer replacementSlot;
    assert(replacementSlot.Initialize(device, functions));
    const auto moved = replacementSlot.Watch(Begin(7), 1, registry);
    assert(moved > serial && !replacementSlot.Ready(serial, 2));
    Clean(replacementSlot);

    // New identity even when a device and buffer handle are reused.
    assert(observer.Initialize(device, functions));
    auto next = observer.Watch(Begin(7), 2, registry);
    assert(next > serial && !observer.Ready(serial, 3));
    registry.End(7);
    Submit(observer, 1, { 7 }, VK_ERROR_OUT_OF_HOST_MEMORY);
    Submit(observer, 1, { 7 });
    assert(!observer.Ready(next, 3) && !creates); // a failed submit never recovers by retry
    Clean(observer);

    for (bool cancelled : { false, true })
    {
        assert(observer.Initialize(device, functions));
        next = observer.Watch(Begin(7), 1, registry);
        registry.End(7);
        if (cancelled)
            observer.Cancel(next);
        else
            createResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        Submit(observer, 1, { 7 });
        assert(!observer.Ready(next, 2) && !markers && !destroys && fences.empty());
        assert(creates == (cancelled ? 0u : 1u));
        Clean(observer);
    }

    for (bool afterComplete : { false, true })
    {
        assert(observer.Initialize(device, functions));
        next = observer.Watch(Begin(7), 1, registry);
        registry.End(7);
        Submit(observer, 1, { 7 });
        if (afterComplete)
        {
            fences.begin()->second.signalled = true;
            assert(observer.Ready(next, 2));
        }
        Submit(observer, 2, { 7 }); // replay on another queue invalidates both pending and complete evidence
        assert(!observer.Ready(next, 3) && creates == 1);
        Clean(observer);
    }
    assert(observer.Initialize(device, functions));
    next = observer.Watch(Begin(7), 1, registry);
    registry.End(7);
    Submit(observer, 1, { 7, 7 });
    assert(!observer.Ready(next, 2) && !creates);
    Clean(observer);

    // Two independent queues completing in reverse order never borrow each other's proof.
    assert(observer.Initialize(device, functions));
    auto first = observer.Watch(Begin(7), 1, registry);
    auto second = observer.Watch(Begin(8), 1, registry);
    registry.End(7);
    registry.End(8);
    Submit(observer, 1, { 7 });
    Submit(observer, 2, { 8 });
    for (auto& [fence, state] : fences)
        if (state.queue == H<VkQueue>(2))
            state.signalled = true;
    assert(observer.Ready(second, 2) && !observer.Ready(first, 2));
    Clean(observer);

    // Step-3 boundary: creation completion is not evaluation queue affinity or completion.
    // A later, unwatched primary recording can be forwarded on another queue while the
    // original creation remains Ready. Its serial says nothing about that new work.
    assert(observer.Initialize(device, functions));
    const auto createdOnFirst = observer.Watch(Begin(7), 1, registry);
    registry.End(7);
    Submit(observer, 1, { 7 });
    fences.begin()->second.signalled = true;
    assert(observer.Ready(createdOnFirst, 2));
    const auto laterRecording = Begin(8);
    assert(laterRecording.level == VkRecording::Level::Primary && laterRecording.oneTime);
    registry.End(8);
    const auto beforeLater = markers;
    Submit(observer, 2, { 8 });
    assert(markers == beforeLater && fences.empty());
    assert(observer.Ready(createdOnFirst, 3));
    Clean(observer);
    std::cout << "PASS: creation readiness does not establish later evaluation queue ownership/order\n";

    // Discard/free/reuse cannot turn an old marker into proof for a replacement recording.
    for (int discard = 0; discard < 5; ++discard)
    {
        assert(observer.Initialize(device, functions));
        next = observer.Watch(Begin(7), 1, registry);
        registry.End(7);
        Submit(observer, 1, { 7 });
        if (discard == 0)
            registry.Reset(7);
        if (discard == 1)
            registry.Free(9, 8, 7);
        if (discard == 2)
            registry.ResetPool(9, 8);
        if (discard == 3)
            registry.DestroyPool(9, 8);
        if (discard == 4)
            registry.Begin(7, VkRecording::Reuse::Serial);
        assert(!observer.Ready(next, 2) && destroys == 0);
        fences.begin()->second.signalled = true;
        assert(!observer.Ready(next, 3) && destroys == 1);
        Clean(observer);
    }
    assert(observer.Initialize(device, functions));
    next = observer.Watch(Begin(7), 1, registry);
    registry.End(7);
    markerResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    Submit(observer, 1, { 7 });
    assert(!observer.Ready(next, 2) && destroys == 0 && fences.size() == 1);
    Clean(observer); // only the explicit drain permits destroying the uncertain failed marker

    for (int lostAt = 0; lostAt < 5; ++lostAt)
    {
        assert(observer.Initialize(device, functions));
        next = observer.Watch(Begin(7), 1, registry);
        registry.End(7);
        if (lostAt == 0)
            Submit(observer, 1, { 7 }, VK_ERROR_DEVICE_LOST);
        if (lostAt == 1)
        {
            markerResult = VK_ERROR_DEVICE_LOST;
            Submit(observer, 1, { 7 });
        }
        if (lostAt == 2)
        {
            Submit(observer, 1, { 7 });
            pollResult = VK_ERROR_DEVICE_LOST;
        }
        if (lostAt == 4)
        {
            createResult = VK_ERROR_DEVICE_LOST;
            Submit(observer, 1, { 7 });
        }
        if (lostAt == 3)
        {
            Submit(observer, 1, { 7 });
            observer.Lost();
            driverLost = true;
        }
        assert(!observer.Ready(next, 2) && destroys == 0);
        observer.Destroy(true); // even a later optimistic drain cannot override device loss
        assert(destroys == 0);
        Clean(observer);
    }
    assert(observer.Initialize(device, functions));
    next = observer.Watch(Begin(7), 1, registry);
    registry.End(7);
    Submit(observer, 1, { 7 });
    const auto locks = CountingMutex::locks;
    const auto work = creates + markers + polls + destroys;
    observer.Off();
    assert(CountingMutex::locks == locks && creates + markers + polls + destroys == work);
    fences.begin()->second.signalled = true;
    assert(!observer.Ready(next, 2));
    Clean(observer);

    assert(observer.Initialize(device, functions));
    for (uint64_t b = 1; b <= 10; ++b)
        assert(observer.Watch(Begin(b), 1, registry));
    assert(!observer.Watch(Begin(11), 1, registry) && !creates); // strict per-device bound
    Clean(observer);
    std::cout
        << "PASS: owned Vulkan creation completion, replay, failure, off interval, bounded lifetime and abandonment\n";
}
