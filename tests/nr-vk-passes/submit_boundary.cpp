#include "pch.h"
#include "DlssNr_VkCreation.h"
#include "DlssNr_VkCoverage.h"
#include <string_view>
#include <vulkan/vulkan.h>
#include <array>
#include <cassert>
#include <iostream>
#include <cstdlib>
#include <new>

bool forbidHeap = false, throwOnHeap = false;
void* operator new(std::size_t bytes)
{
    assert(!forbidHeap);
    if (throwOnHeap)
        throw std::bad_alloc();
    if (auto* value = std::malloc(bytes ? bytes : 1))
        return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

// Execute production queue wrappers with a driver boundary fake. This characterizes the
// missing adapter; a passing test does NOT mean submission/completion is implemented.
using namespace DlssNr;
struct ObserverMutex
{
    inline static unsigned calls = 0, expectedDepth = 1, constructed = 0;
    ObserverMutex() { ++constructed; }
    PFN_vkQueueSubmit2 nestedCallback = nullptr;
    bool inNestedCallback = false;
    bool held = false;
    void lock()
    {
        assert(!held);
        held = true;
        ++calls;
    }
    void unlock() { held = false; }
};
using CreationObserver = VkChain::CreationObserver<ObserverMutex>;
CreationObserver* queueObserver = nullptr;

ObserverMutex g_installMutex;
unsigned resolves = 0;
struct Config
{
    struct Option
    {
        uint32_t value;
        uint32_t value_or_default() const { return value; }
    };
    Option DlssNrEnabled { 1 }, DlssNrPasses { 1 };
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};
template <class T> uint64_t H(T value) { return (uint64_t) value; }
template <class T> T As(uint64_t value) { return (T) value; }
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
QueueLocks* QueueLocksFor(VkQueue queue, CreationObserver** observer = nullptr)
{
    if (observer)
        *observer = queueObserver;
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
unsigned calls = 0, expectedDepth = 1;
PFN_vkQueueSubmit2 nestedCallback = nullptr;
bool inNestedCallback = false;

template <class Info> VkResult Forward(VkQueue queue, uint32_t count, const Info* info, VkFence fence)
{
    assert(queue == expectedQueue && fence == expectedFence && count == 1 && info == expectedSubmits);
    if (auto* locks = QueueLocksFor(queue))
        assert(locks->submit.held == expectedDepth);
    // Reenter the REAL recording registry from the fake driver: queue callbacks cannot hold its mutex.
    assert(!VkRecording::Recordings().Current(9, 7)); // executable, not open
    ++calls;
    return driverResult;
}
VkResult VKAPI_CALL Submit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) { return Forward(q, n, s, f); }
VkResult VKAPI_CALL Submit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f)
{
    if (nestedCallback && !inNestedCallback)
    {
        inNestedCallback = true;
        const auto result = nestedCallback(q, n, s, f);
        inNestedCallback = false;
        return result;
    }
    return Forward(q, n, s, f);
}
template <class Pfn> struct Targets
{
    std::array<Pfn, 4> next {};
};
Targets<PFN_vkQueueSubmit> g_submit { { Submit, Submit } };
Targets<PFN_vkQueueSubmit2> g_submit2 { { Submit2, Submit2 } };

unsigned poolCalls = 0;
const VkCommandPoolCreateInfo* expectedPoolInfo = nullptr;
VkCommandPool* expectedPoolOut = nullptr;
VkResult VKAPI_CALL PoolCreate(VkDevice device, const VkCommandPoolCreateInfo* info, const VkAllocationCallbacks* alloc,
                               VkCommandPool* pool)
{
    assert(device == Handle<VkDevice>(20) && info == expectedPoolInfo && pool == expectedPoolOut &&
           info->queueFamilyIndex == 3 && !alloc);
    ++poolCalls;
    *pool = Handle<VkCommandPool>(200);
    return VK_SUCCESS;
}

unsigned ownCreates = 0, ownSubmits = 0, ownPolls = 0, ownDestroys = 0;
VkResult VKAPI_CALL OwnCreate(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence)
{
    ++ownCreates;
    *fence = Handle<VkFence>(100);
    return VK_SUCCESS;
}
VkResult VKAPI_CALL OwnSubmit(VkQueue q, uint32_t count, const VkSubmitInfo* info, VkFence fence)
{
    assert(q == expectedQueue && count == 1 && info && !info->commandBufferCount && fence != expectedFence);
    ++ownSubmits;
    return VK_SUCCESS;
}
VkResult VKAPI_CALL OwnStatus(VkDevice, VkFence fence)
{
    assert(fence == Handle<VkFence>(100));
    ++ownPolls;
    return VK_SUCCESS;
}
void VKAPI_CALL OwnDestroy(VkDevice, VkFence fence, const VkAllocationCallbacks*)
{
    assert(fence == Handle<VkFence>(100));
    ++ownDestroys;
}

template <class Pfn> bool Resolve(VkDevice, const char* name, Pfn& out)
{
    assert(!g_installMutex.held);
    ++resolves;
    if constexpr (std::is_same_v<Pfn, PFN_vkCreateFence>)
    {
        assert(std::string_view(name) == "vkCreateFence");
        out = OwnCreate;
    }
    else if constexpr (std::is_same_v<Pfn, PFN_vkDestroyFence>)
    {
        assert(std::string_view(name) == "vkDestroyFence");
        out = OwnDestroy;
    }
    else if constexpr (std::is_same_v<Pfn, PFN_vkGetFenceStatus>)
    {
        assert(std::string_view(name) == "vkGetFenceStatus");
        out = OwnStatus;
    }
    else
        return false;
    return true;
}

#define LOG_INFO(...) (++budgetLogs)
unsigned budgetLogs = 0;
// @production@

int main()
{
    VkDeviceCreateInfo deviceInfo {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    auto device = Handle<VkDevice>(20);
    const auto beforeRegistration = ObserverMutex::calls;
    assert(ObserverMutex::constructed == 1); // existing install mutex only; no chain mutex constructed
    forbidHeap = true;
    RegisterCreationObserver(device, &deviceInfo, OwnSubmit);
    assert(!resolves && ObserverMutex::calls == beforeRegistration && !CreationObserverFor(device));
    assert(ObserverMutex::constructed == 1);
    forbidHeap = false;
    assert(EnsureChainState());
    assert(ObserverMutex::constructed > 1); // constructed only on an explicit startup request
    g_chainState.load()->createPool.next[0] = PoolCreate;
    assert(g_chainState.load()->coverage.Publish(20, { VkChain::HookCoverage::Required }));
    VkDeviceQueueCreateInfo protectedQueue {};
    protectedQueue.flags = VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &protectedQueue;
    RegisterCreationObserver(device, &deviceInfo, OwnSubmit);
    assert(!resolves && !CreationObserverFor(device));
    deviceInfo.queueCreateInfoCount = 0;
    VkDeviceGroupDeviceCreateInfo group {};
    group.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO;
    group.physicalDeviceCount = 2;
    deviceInfo.pNext = &group;
    RegisterCreationObserver(device, &deviceInfo, OwnSubmit);
    assert(!resolves && !CreationObserverFor(device));
    deviceInfo.pNext = nullptr;
    RegisterCreationObserver(device, &deviceInfo, OwnSubmit);
    assert(resolves == 3 && CreationObserverFor(device));
    RegisterCreationObserver(device, &deviceInfo, OwnSubmit);
    assert(resolves == 3); // duplicate notification cannot allocate another observer slot
    auto& order = CreationObserverFor(device)->Order();
    order.Configure({ 1, 3, true });
    order.Queue(Handle<VkQueue>(1), 3, 0);
    VkCommandPoolCreateInfo poolInfo {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = 3;
    VkCommandPool pool;
    expectedPoolInfo = &poolInfo;
    expectedPoolOut = &pool;
    Config::Instance()->DlssNrPasses.value = 1;
    auto poolLocks = ObserverMutex::calls;
    forbidHeap = true;
    assert(hkChainCreateCommandPool<0>(device, &poolInfo, nullptr, &pool) == VK_SUCCESS);
    assert(poolCalls == 1 && ObserverMutex::calls == poolLocks);
    forbidHeap = false;
    assert(!order.Accept(pool));
    order.Configure({ 1, 3, true });
    order.Queue(Handle<VkQueue>(1), 3, 0);
    Config::Instance()->DlssNrPasses.value = 2;
    assert(hkChainCreateCommandPool<0>(device, &poolInfo, nullptr, &pool) == VK_SUCCESS && poolCalls == 2);
    assert(order.Accept(pool));
    auto& poolRegistry = VkRecording::Recordings();
    poolRegistry.Allocate(20, 200, { 50 }, VkRecording::Level::Primary);
    poolRegistry.Begin(50, VkRecording::Reuse::Serial, true);
    const auto poolTicket = *poolRegistry.Current(20, 50);
    assert(ChainRecordingAllowed(device, poolTicket));
    auto stalePoolTicket = poolTicket;
    ++stalePoolTicket.epoch;
    assert(!poolRegistry.PoolOf(stalePoolTicket));
    poolRegistry.ForgetDevice(20);
    poolLocks = ObserverMutex::calls;
    Config::Instance()->DlssNrPasses.value = 1;
    forbidHeap = true;
    ForgetChainPool(device, pool);
    assert(ObserverMutex::calls == poolLocks);
    forbidHeap = false;
    assert(!order.Accept(pool));
    VkChain::BudgetGate budgetGate;
    VkChain::BudgetKey budgetKey;
    budgetKey.count = 2;
    Config::Instance()->DlssNrPasses.value = 1;
    const auto beforeBudget = ObserverMutex::calls;
    forbidHeap = true;
    assert(AdmitChainBudget(device, budgetGate, budgetKey, 0, {}).legacy);
    assert(!ChainRecordingAllowed(device, {}));
    assert(SampleChainBudget(device).reason == VkChain::BudgetReason::Query);
    assert(ObserverMutex::calls == beforeBudget && budgetLogs == 0);
    std::vector<std::string> extensionNames;
    assert(!VkChain::EnableBudgetExtension(false, true, extensionNames, extensionNames, extensionNames));
    forbidHeap = false;
    Config::Instance()->DlssNrPasses.value = 2;
    assert(AdmitChainBudget(device, budgetGate, budgetKey, 0, {}).reason == VkChain::BudgetReason::Extension);
    assert(budgetLogs == 1);
    assert(AdmitChainBudget(device, budgetGate, budgetKey, 0, {}).reason == VkChain::BudgetReason::Extension);
    assert(budgetLogs == 1);
    ++budgetKey.width;
    assert(AdmitChainBudget(device, budgetGate, budgetKey, 0, {}).reason == VkChain::BudgetReason::Extension);
    assert(budgetLogs == 2);
    CreationObserverFor(device)->Destroy(true);
    g_chainState.store(nullptr);
    g_chainOwner.reset();

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
    for (uint32_t passes : { 1, 2 })
    {
        Config::Instance()->DlssNrPasses.value = passes;
        for (uint64_t queue : { 1, 2, 3 }) // two known queues and an unobserved queue
            for (auto result : { VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_DEVICE_LOST })
                for (unsigned api = 0; api < 3; ++api)
                {
                    driverResult = result;
                    expectedQueue = Handle<VkQueue>(queue);
                    expectedFence = Handle<VkFence>(10); // borrowed/recycled application fence, never proof
                    expectedSubmits = api == 0 ? static_cast<void*>(&legacy) : static_cast<void*>(&modern);
                    const auto before = calls;
                    forbidHeap = true;
                    const auto observed = api == 0   ? hkQueueSubmit<0>(expectedQueue, 1, &legacy, expectedFence)
                                          : api == 1 ? hkQueueSubmit2<0>(expectedQueue, 1, &modern, expectedFence)
                                                     : hkQueueSubmit2<1>(expectedQueue, 1, &modern, expectedFence);
                    forbidHeap = false;
                    assert(observed == result && calls == before + 1);
                    assert(queueLocks[0].submit.held == 0 && queueLocks[1].submit.held == 0);
                    assert(pin.use_count() == 2); // none of these outcomes retires an executable recording
                    assert(!creation.Ready(100)); // neither success nor a fence handle feeds the creation proof
                }
    }
    assert(!g_chainState.load()); // all 54 default forwards ran without chain tables or mutexes
    assert(EnsureChainState());
    // An already armed device with Passes=1 still adds zero observer locks and zero Vulkan calls.
    for (uint32_t passes : { 1, 2 })
        for (unsigned api = 0; api < 3; ++api)
        {
            CreationObserver observer;
            assert(observer.Initialize(Handle<VkDevice>(9), { OwnCreate, OwnDestroy, OwnStatus, OwnSubmit }));
            registry.Begin(7, VkRecording::Reuse::Serial, true);
            throwOnHeap = true;
            const auto refusedOom = observer.Watch(*registry.Current(9, 7), 1, registry);
            throwOnHeap = false;
            assert(!refusedOom);
            auto tracked = observer.Watch(*registry.Current(9, 7), 1, registry);
            assert(tracked);
            registry.End(7);
            queueObserver = &observer;
            Config::Instance()->DlssNrPasses.value = passes;
            expectedQueue = Handle<VkQueue>(1);
            expectedFence = Handle<VkFence>(10);
            expectedSubmits = api == 0 ? static_cast<void*>(&legacy) : static_cast<void*>(&modern);
            driverResult = VK_SUCCESS;
            const auto before = calls;
            const auto locks = ObserverMutex::calls;
            const auto owned = ownCreates + ownSubmits + ownPolls + ownDestroys;
            nestedCallback = passes == 2 && api == 2 ? hkQueueSubmit2<1> : nullptr;
            expectedDepth = nestedCallback ? 2 : 1;
            forbidHeap = passes == 1;
            const auto result = api == 0   ? hkQueueSubmit<0>(expectedQueue, 1, &legacy, expectedFence)
                                : api == 1 ? hkQueueSubmit2<0>(expectedQueue, 1, &modern, expectedFence)
                                           : hkQueueSubmit2<1>(expectedQueue, 1, &modern, expectedFence);
            forbidHeap = false;
            assert(result == VK_SUCCESS && calls == before + 1);
            if (passes == 1)
            {
                assert(ObserverMutex::calls == locks);
                assert(ownCreates + ownSubmits + ownPolls + ownDestroys == owned);
                assert(!observer.Ready(tracked, 2));
            }
            else
                assert(observer.Ready(tracked, 2));
            nestedCallback = nullptr;
            expectedDepth = 1;
            observer.Destroy(false);
            registry.Reset(7);
            queueObserver = nullptr;
        }
    registry.Reset(7);
    assert(pin.use_count() == 1 && !creation.Ready(100));
    registry.ForgetDevice(9);
    std::cout << "PASS: 54 inactive forwarding cases plus armed off/active submit, submit2 and shared KHR callback\n";
}
