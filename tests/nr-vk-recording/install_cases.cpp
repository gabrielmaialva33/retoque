#include "pch.h"
#include <dlssnr/DlssNr_VkCoverage.h>
// @common-fakes@

#include <string_view>
#include <unordered_set>

#define LOG_INFO(...) Log(__VA_ARGS__)
#define VKAPI_CALL
using PFN_vkVoidFunction = void (*)();
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction (*)(VkDevice, const char*);
using PFN_vkAllocateCommandBuffers = decltype(&DriverAllocate);
using PFN_vkBeginCommandBuffer = decltype(&DriverBegin);
using PFN_vkEndCommandBuffer = decltype(&DriverEnd);
using PFN_vkResetCommandBuffer = decltype(&DriverReset);
using PFN_vkFreeCommandBuffers = decltype(&DriverFree);
using PFN_vkResetCommandPool = decltype(&DriverResetPool);
using PFN_vkDestroyCommandPool = decltype(&DriverDestroyPool);
using HMODULE = void*;
using PVOID = void*;
using LONG = long;
constexpr LONG NO_ERROR = 0;

struct CheckingMutex
{
    bool held = false;
    void lock()
    {
        Check(!held, "installation mutex must not be recursively acquired");
        held = true;
    }
    void unlock() { held = false; }
} g_installMutex;
std::unordered_set<void*> g_attached;
HMODULE g_module = reinterpret_cast<HMODULE>(1);
std::unordered_map<void*, void*> installed;
void* pendingTarget = nullptr;
void* pendingHook = nullptr;
LONG commitResult = NO_ERROR;
LONG beginResult = NO_ERROR, updateResult = NO_ERROR, attachResult = NO_ERROR;
int aborts = 0;
bool coverQueues = false;
DlssNr::VkChain::CoverageRegistry g_chainCoverage;
int rejectedOperation = -1;
int lookups = 0;
int exportKind = 0;
int admissions = 0;
bool ownBeginAddress = false;
std::array<int, 4> deviceKinds { 0, 1, 2, 3 };
std::array<std::array<int, 4>, 7> calls {};
constexpr std::array<const char*, 7> names { "vkAllocateCommandBuffers", "vkBeginCommandBuffer", "vkEndCommandBuffer",
                                             "vkResetCommandBuffer",     "vkFreeCommandBuffers", "vkResetCommandPool",
                                             "vkDestroyCommandPool" };

void Routed(size_t operation, size_t kind)
{
    Check(!g_installMutex.held, "driver callback must run outside installation lock");
    ++calls[operation][kind];
}
template <size_t K> VkResult AllocateAt(VkDevice d, const VkCommandBufferAllocateInfo* i, VkCommandBuffer* b)
{
    Routed(0, K);
    return DriverAllocate(d, i, b);
}
template <size_t K> VkResult BeginAt(VkCommandBuffer b, const VkCommandBufferBeginInfo* i)
{
    Routed(1, K);
    return DriverBegin(b, i);
}
template <size_t K> VkResult EndAt(VkCommandBuffer b)
{
    Routed(2, K);
    return DriverEnd(b);
}
template <size_t K> VkResult ResetAt(VkCommandBuffer b, VkCommandBufferResetFlags f)
{
    Routed(3, K);
    return DriverReset(b, f);
}
template <size_t K> void FreeAt(VkDevice d, VkCommandPool p, uint32_t c, const VkCommandBuffer* b)
{
    Routed(4, K);
    DriverFree(d, p, c, b);
}
template <size_t K> VkResult ResetPoolAt(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f)
{
    Routed(5, K);
    return DriverResetPool(d, p, f);
}
template <size_t K> void DestroyPoolAt(VkDevice d, VkCommandPool p, const VkAllocationCallbacks* a)
{
    Routed(6, K);
    DriverDestroyPool(d, p, a);
}

#define ADDRESSES(function)                                                                                            \
    std::array<PFN_vkVoidFunction, 4>                                                                                  \
    {                                                                                                                  \
        reinterpret_cast<PFN_vkVoidFunction>(&function<0>), reinterpret_cast<PFN_vkVoidFunction>(&function<1>),        \
            reinterpret_cast<PFN_vkVoidFunction>(&function<2>), reinterpret_cast<PFN_vkVoidFunction>(&function<3>)     \
    }
const std::array<std::array<PFN_vkVoidFunction, 4>, 7> addresses { ADDRESSES(AllocateAt),   ADDRESSES(BeginAt),
                                                                   ADDRESSES(EndAt),        ADDRESSES(ResetAt),
                                                                   ADDRESSES(FreeAt),       ADDRESSES(ResetPoolAt),
                                                                   ADDRESSES(DestroyPoolAt) };
#undef ADDRESSES

void OwnBegin() {}
void Extra0() {}
void Extra1() {}
void Extra2() {}
void Extra3() {}
void Extra4() {}
constexpr std::array<const char*, 6> extraNames { "vkDestroyDevice", "vkGetDeviceQueue", "vkGetDeviceQueue2",
                                                  "vkQueueSubmit",   "vkQueueSubmit2",   "vkQueueSubmit2KHR" };
const std::array<PFN_vkVoidFunction, 6> extraAddresses { Extra0, Extra1, Extra2, Extra3, Extra4, Extra4 };
bool InOwnModule(const void* address) { return address == reinterpret_cast<const void*>(&OwnBegin); }
PFN_vkVoidFunction Lookup(const char* name, int kind)
{
    Check(!g_installMutex.held, "procedure lookup must happen before taking installation lock");
    ++lookups;
    if (coverQueues)
        for (size_t i = 0; i < extraNames.size(); ++i)
            if (std::string_view(name) == extraNames[i])
                return extraAddresses[i];
    for (size_t operation = 0; operation < names.size(); ++operation)
        if (std::string_view(name) == names[operation])
            return operation == 1 && ownBeginAddress ? &OwnBegin : addresses[operation][kind];
    return nullptr;
}
PFN_vkVoidFunction DeviceLookup(VkDevice d, const char* name) { return Lookup(name, deviceKinds[Key(d)]); }
PFN_vkVoidFunction ExportLookup(HMODULE, const char* name) { return Lookup(name, exportKind); }
PFN_vkGetDeviceProcAddr g_getDeviceProcAddr = DeviceLookup;
namespace VulkanHooks
{
PFN_vkVoidFunction GetDeviceProcAddr(VkDevice d, const char* name) { return DeviceLookup(d, name); }
} // namespace VulkanHooks
namespace KernelBaseProxy
{
auto GetProcAddress_() { return &ExportLookup; }
} // namespace KernelBaseProxy

void* GetCurrentThread() { return nullptr; }
LONG DetourTransactionBegin()
{
    pendingTarget = pendingHook = nullptr;
    return beginResult;
}
void DetourTransactionAbort()
{
    pendingTarget = pendingHook = nullptr;
    ++aborts;
}
LONG DetourUpdateThread(void*) { return updateResult; }
LONG DetourAttach(void** next, void* hook)
{
    Check(g_installMutex.held, "detour bookkeeping must hold installation lock");
    pendingTarget = *next;
    pendingHook = hook;
    return attachResult;
}
LONG DetourTransactionCommit()
{
    const bool rejected = rejectedOperation >= 0 &&
                          std::any_of(addresses[rejectedOperation].begin(), addresses[rejectedOperation].end(),
                                      [](auto address) { return reinterpret_cast<void*>(address) == pendingTarget; });
    if (commitResult == NO_ERROR && !rejected && pendingTarget != nullptr)
        installed[pendingTarget] = pendingHook;
    pendingTarget = pendingHook = nullptr;
    return rejected ? 42 : commitResult;
}

struct Config
{
    struct Option
    {
        bool value = true;
        bool value_or_default() const { return value; }
    };
    struct Count
    {
        uint32_t value = 1;
        uint32_t value_or_default() const { return value; }
    };
    Count DlssNrPasses;
    Option DlssNrEnabled;
    Option DlssNrVulkanPresentHost;
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};

// @production@

// @common-helpers@

template <class Pfn> Pfn Through(size_t operation, int kind)
{
    auto original = reinterpret_cast<void*>(addresses[operation][kind]);
    const auto found = installed.find(original);
    return reinterpret_cast<Pfn>(found == installed.end() ? original : found->second);
}
void FreshInstall()
{
    Fresh();
    for (uint64_t d = 1; d <= 32; ++d)
        g_chainCoverage.Forget(d);
    coverQueues = false;
    beginResult = updateResult = attachResult = NO_ERROR;
    aborts = 0;
    Config::Instance()->DlssNrPasses.value = 1;
    R().ForgetDevice(2);
    R().ForgetDevice(3);
    ResetTargetSets();
    installed.clear();
    g_attached.clear();
    calls = {};
    lookups = admissions = 0;
    exportKind = 0;
    deviceKinds = { 0, 1, 2, 3 };
    ownBeginAddress = false;
    commitResult = NO_ERROR;
    rejectedOperation = -1;
    g_module = reinterpret_cast<HMODULE>(1);
    g_getDeviceProcAddr = DeviceLookup;
    Config::Instance()->DlssNrEnabled.value = true;
    Config::Instance()->DlssNrVulkanPresentHost.value = true;
}
VkCommandBuffer AllocateThrough(VkDevice d, int kind)
{
    VkCommandBufferAllocateInfo info {};
    info.commandPool = pool;
    info.commandBufferCount = 1;
    VkCommandBuffer buffer = nullptr;
    Check(Through<PFN_vkAllocateCommandBuffers>(0, kind)(d, &info, &buffer) == VK_SUCCESS,
          "allocation observer must preserve driver result");
    return buffer;
}
Ticket BeginThrough(VkDevice d, VkCommandBuffer buffer, int kind)
{
    VkCommandBufferBeginInfo info {};
    Check(Through<PFN_vkBeginCommandBuffer>(1, kind)(buffer, &info) == VK_SUCCESS, "Begin must preserve driver result");
    auto ticket = R().Current(Key(d), Key(buffer));
    Check(ticket.has_value(), "independent observer must admit a known recording without wDx12");
    return *ticket;
}

void InstallCoversExportAndProcedure()
{
    HookDevice(device);
    for (size_t operation = 0; operation < names.size(); ++operation)
        for (int kind : { 0, 1 })
            Check(installed.count(reinterpret_cast<void*>(addresses[operation][kind])) == 1,
                  "host must observe every lifecycle export and device procedure independently of wDx12");
    Check(installed.size() == 14, "seven different export/procedure pairs must each attach once");
    const int before = lookups;
    auto buffer = AllocateThrough(device, 0);
    auto resource = Pin(BeginThrough(device, buffer, 1));
    Check(Through<PFN_vkEndCommandBuffer>(2, 0)(buffer) == VK_SUCCESS, "End must preserve success");
    Check(!R().Current(Key(device), Key(buffer)) && !resource.expired(), "End must close admission and retain pins");
    driver.probe = resource;
    Through<PFN_vkFreeCommandBuffers>(4, 1)(device, pool, 1, &buffer);
    Check(driver.aliveAtForward && resource.expired(), "Free must retain pins through the real call");
    Check(calls[0][0] == 1 && calls[1][1] == 1 && calls[2][0] == 1 && calls[4][1] == 1,
          "each observer must call its selected trampoline exactly once");
    Check(lookups == before, "observers must not resolve functions or reacquire installation locks");
}

void MultipleTargetsAndAliases()
{
    exportKind = 1;
    HookDevice(device);
    Check(installed.size() == 7, "identical export and device addresses must be attached once");
    HookDevice(device);
    Check(installed.size() == 7, "repeat installation must not stack another detour");
    auto second = H<VkDevice>(2);
    HookDevice(second);
    Check(installed.size() == 14, "another ICD address must receive its own target slot");
    auto buffer = AllocateThrough(second, 2);
    auto ticket = BeginThrough(second, buffer, 2);
    Check(ticket.device == 2 && calls[0][2] == 1 && calls[1][2] == 1,
          "second target slot must forward to its own driver and device");
    Through<PFN_vkFreeCommandBuffers>(4, 2)(second, pool, 1, &buffer);
    R().Begin(Key(buffer));
    Check(!R().Current(2, Key(buffer)), "free through another target must remove that allocation");
}

void ObserverSuccessBoundaries()
{
    HookDevice(device);
    auto buffer = AllocateThrough(device, 1);
    auto old = BeginThrough(device, buffer, 0);
    auto resource = Pin(old);
    driver.probe = resource;
    VkCommandBufferBeginInfo begin {};
    driver.beginResult = VK_ERROR_UNKNOWN;
    Check(Through<PFN_vkBeginCommandBuffer>(1, 1)(buffer, &begin) == VK_ERROR_UNKNOWN,
          "failed Begin result must remain unchanged");
    Check(!resource.expired() && R().Current(1, Key(buffer))->epoch == old.epoch,
          "failed Begin observer must preserve pins and epoch");
    driver.endResult = VK_ERROR_UNKNOWN;
    Through<PFN_vkEndCommandBuffer>(2, 1)(buffer);
    Check(R().Current(1, Key(buffer)).has_value() && !resource.expired(), "failed End must preserve recording");
    driver.resetResult = VK_ERROR_UNKNOWN;
    Through<PFN_vkResetCommandBuffer>(3, 1)(buffer, 0);
    Check(!resource.expired(), "failed reset must preserve pins");
    driver.resetResult = VK_SUCCESS;
    Through<PFN_vkResetCommandBuffer>(3, 0)(buffer, 0);
    Check(driver.aliveAtForward && resource.expired(), "successful reset must release only after forwarding");

    driver.beginResult = VK_SUCCESS;
    resource = Pin(BeginThrough(device, buffer, 1));
    driver.probe = resource;
    driver.resetPoolResult = VK_ERROR_UNKNOWN;
    Through<PFN_vkResetCommandPool>(5, 0)(device, pool, 0);
    Check(!resource.expired(), "failed pool reset must preserve pins");
    driver.resetPoolResult = VK_SUCCESS;
    Through<PFN_vkResetCommandPool>(5, 1)(device, pool, 0);
    Check(driver.aliveAtForward && resource.expired(), "pool reset must release after success");
    resource = Pin(BeginThrough(device, buffer, 1));
    driver.probe = resource;
    Through<PFN_vkDestroyCommandPool>(6, 0)(device, pool, nullptr);
    Check(driver.aliveAtForward && resource.expired(), "pool destruction must release after driver call");
    R().Begin(Key(buffer));
    Check(!R().Current(1, Key(buffer)), "pool destruction must remove allocation records");
}

// The independent observers pass Begin's flags on as the wDx12 wrapper does: the zero-motion probe reads them.
void ObserverBeginFlagsReachTheTicket()
{
    HookDevice(device);
    auto buffer = AllocateThrough(device, 0);
    VkCommandBufferBeginInfo begin {};
    Check(Through<PFN_vkBeginCommandBuffer>(1, 0)(buffer, &begin) == VK_SUCCESS, "Begin must pass");
    Check(R().Current(Key(device), Key(buffer))->reuse == Reuse::Serial, "a Begin without SIMULTANEOUS_USE is serial");
    begin.flags = kSimultaneousUseBit;
    Check(Through<PFN_vkBeginCommandBuffer>(1, 1)(buffer, &begin) == VK_SUCCESS, "Begin must pass");
    Check(R().Current(Key(device), Key(buffer))->reuse == Reuse::Simultaneous,
          "a Begin with SIMULTANEOUS_USE is simultaneous");
    Check(Through<PFN_vkBeginCommandBuffer>(1, 0)(buffer, nullptr) == VK_SUCCESS, "Begin must pass");
    Check(R().Current(Key(device), Key(buffer))->reuse == Reuse::Unknown, "a Begin without its info stays unknown");
    Through<PFN_vkFreeCommandBuffers>(4, 0)(device, pool, 1, &buffer);
}

void InstallRemainsOptIn()
{
    Config::Instance()->DlssNrEnabled.value = false;
    ObserveInstallGate(g_module);
    Config::Instance()->DlssNrEnabled.value = true;
    Config::Instance()->DlssNrVulkanPresentHost.value = false;
    ObserveInstallGate(g_module);
    Config::Instance()->DlssNrVulkanPresentHost.value = true;
    ObserveInstallGate(nullptr);
    Check(admissions == 0 && installed.empty() && lookups == 0, "off/default host must end before lookup or detour");
    ObserveInstallGate(g_module);
    Check(admissions == 1, "enabled host with module must reach installation");
}

void FailedDetourAndOwnAddresses()
{
    commitResult = 42;
    HookDevice(device);
    Check(installed.empty() && g_attached.empty(), "failed transactions must not publish targets");
    auto buffer = AllocateThrough(device, 1);
    VkCommandBufferBeginInfo begin {};
    Through<PFN_vkBeginCommandBuffer>(1, 1)(buffer, &begin);
    Check(!R().Current(1, Key(buffer)), "unobserved allocation must stay inadmissible after detour failure");
    commitResult = NO_ERROR;
    HookDevice(device);
    buffer = AllocateThrough(device, 1);
    BeginThrough(device, buffer, 1);
    Check(installed.size() == 14, "later successful installation must recover all target slots");
    FreshInstall();
    ownBeginAddress = true;
    HookDevice(device);
    Check(installed.count(reinterpret_cast<void*>(&OwnBegin)) == 0,
          "function lookup returning our module must not recursively detour its own observer");
    Check(installed.size() == 12, "other lifecycle functions must still install when Begin is unavailable");
}

// Characterization of the step-1 blocker, not a safe completion adapter. Successful Begin does
// not certify that all retirement entrypoints installed. Keep this witness until admission has
// a per-device coverage result, then replace it with an assertion that such admission is refused.
void PartialInstallCannotCertifyRecordingLifetime()
{
    Config::Instance()->DlssNrPasses.value = 2;
    rejectedOperation = 3; // both ResetCommandBuffer addresses fail; all other hooks install
    const auto coverage = HookDevice(device);
    Check(!coverage.Complete() && !g_chainCoverage.Ready(Key(device)),
          "partial installation must deny chain admission");
    Check(installed.size() == 12, "partial install must leave only reset unwatched in this witness");
    auto buffer = AllocateThrough(device, 1);
    auto ticket = BeginThrough(device, buffer, 1);
    auto resource = Pin(ticket);
    Check(Through<PFN_vkEndCommandBuffer>(2, 1)(buffer) == VK_SUCCESS, "End must succeed");
    Check(Through<PFN_vkResetCommandBuffer>(3, 1)(buffer, 0) == VK_SUCCESS, "driver reset must succeed");
    Check(calls[3][1] == 1 && !resource.expired(),
          "a successful unobserved reset leaves a pin behind: pin presence is not coverage proof");
    auto replacement = BeginThrough(device, buffer, 1);
    Check(replacement.epoch != ticket.epoch && resource.expired(),
          "the next observed Begin eventually retires it, but cannot retroactively certify coverage");
}

void SerialSecondaryIsNotPrimaryEvidence()
{
    HookDevice(device);
    for (int level : { 0, 1 }) // primary and secondary have the same serial Begin flags
    {
        VkCommandBufferAllocateInfo info {};
        info.commandPool = pool;
        info.commandBufferCount = 1;
        info.level = level;
        VkCommandBuffer buffer = nullptr;
        Check(Through<PFN_vkAllocateCommandBuffers>(0, 1)(device, &info, &buffer) == VK_SUCCESS,
              "primary/secondary allocation must succeed");
        auto ticket = BeginThrough(device, buffer, 1);
        Check(ticket.reuse == Reuse::Serial, "serial reuse alone cannot distinguish primary from secondary");
        auto resource = Pin(ticket);
        Check(!resource.expired(), "existing registry accepts both levels, so it cannot certify primary creation");
        Through<PFN_vkFreeCommandBuffers>(4, 1)(device, pool, 1, &buffer);
        Check(resource.expired(), "witness cleanup must retire the observed recording");
    }
}

void CoverageCertificates()
{
    coverQueues = true;
    const auto legacy = HookDevice(device);
    Check(legacy.Complete() && !g_chainCoverage.Contains(1), "one pass must not publish any new device certificate");
    Config::Instance()->DlssNrPasses.value = 2;
    const auto complete = HookDevice(device);
    Check(complete.Complete() && g_chainCoverage.Ready(1), "complete required hooks must certify this device");
    Check(!g_chainCoverage.Ready(2), "coverage must not cross devices");
    failAllocation.store(true);
    const bool readyWithoutHeap = g_chainCoverage.Ready(1);
    failAllocation.store(false);
    Check(readyWithoutHeap, "certificate admission must not allocate");
    g_chainCoverage.Forget(1);
    Check(!g_chainCoverage.Ready(1), "device retirement revokes its certificate");
    Check(g_chainCoverage.Publish(1, {}) && !g_chainCoverage.Publish(1, complete) && !g_chainCoverage.Ready(1),
          "a partial live-device certificate cannot be upgraded after observation was lost");
    for (uint64_t d = 2; d <= 16; ++d)
        Check(g_chainCoverage.Publish(d, complete), "bounded certificate slots must admit within capacity");
    Check(!g_chainCoverage.Publish(17, complete) && !g_chainCoverage.Ready(17),
          "capacity exhaustion must deny admission");
}

void TransactionFailuresCannotCertify()
{
    for (auto* failed : { &beginResult, &updateResult, &attachResult })
    {
        FreshInstall();
        coverQueues = true;
        Config::Instance()->DlssNrPasses.value = 2;
        *failed = 42;
        Check(!HookDevice(device).Complete() && !g_chainCoverage.Ready(1) && installed.empty(),
              "every transaction phase must succeed before coverage is certified");
    }
}

void TargetSaturationCannotCertify()
{
    Targets<PFN_vkVoidFunction> targets;
    const std::array<void*, kTargets> hooks {};
    std::lock_guard lock(g_installMutex);
    for (size_t i = 0; i < 4; ++i)
        Check(AttachTarget(targets, hooks, extraAddresses[i], "fixture"), "first four distinct targets must install");
    Check(!AttachTarget(targets, hooks, extraAddresses[4], "fixture"), "fifth target must report failure");
    Check(AttachTarget(targets, hooks, extraAddresses[0], "fixture"), "a known alias still has coverage at capacity");
}

void ObserverAllocationFailure()
{
    HookDevice(device);
    VkCommandBufferAllocateInfo info {};
    info.commandPool = pool;
    info.commandBufferCount = 1;
    VkCommandBuffer buffer = nullptr;
    bool threw = false;
    VkResult result = VK_ERROR_UNKNOWN;
    failAllocation.store(true);
    try
    {
        result = Through<PFN_vkAllocateCommandBuffers>(0, 1)(device, &info, &buffer);
    }
    catch (const std::bad_alloc&)
    {
        threw = true;
    }
    failAllocation.store(false);
    Check(!threw && result == VK_SUCCESS && calls[0][1] == 1,
          "observer allocation failure must preserve driver success and remain inside C boundary");
    R().Begin(Key(buffer));
    Check(!R().Current(1, Key(buffer)), "allocation adapter failure must not invent a registered buffer");
    auto resource = Pin(DirectRecording(buffer));
    driver.probe = resource;
    failAllocation.store(true);
    try
    {
        Through<PFN_vkFreeCommandBuffers>(4, 1)(device, pool, 1, &buffer);
    }
    catch (const std::bad_alloc&)
    {
        threw = true;
    }
    failAllocation.store(false);
    Check(!threw && calls[4][1] == 1 && driver.aliveAtForward && resource.expired(),
          "observer Free must retire after forwarding even when every new allocation fails");
    R().Begin(Key(buffer));
    Check(!R().Current(1, Key(buffer)), "observer Free under OOM must remove the allocation record");
}

void ObserverHandleReuseAfterOutOfMemory()
{
    HookDevice(device);
    auto buffer = AllocateThrough(device, 1);
    const auto old = BeginThrough(device, buffer, 1);
    auto resource = Pin(old);
    driver.probe = resource;
    const auto newPool = H<VkCommandPool>(6);
    VkCommandBufferAllocateInfo info {};
    info.commandPool = newPool;
    info.commandBufferCount = 1;
    const auto recycled = buffer;
    bool threw = false;
    VkResult result = VK_ERROR_UNKNOWN;
    failAllocation.store(true);
    try
    {
        Through<PFN_vkFreeCommandBuffers>(4, 1)(device, pool, 1, &buffer);
        driver.nextBuffer = Key(recycled);
        result = Through<PFN_vkAllocateCommandBuffers>(0, 1)(device, &info, &buffer);
    }
    catch (const std::bad_alloc&)
    {
        threw = true;
    }
    failAllocation.store(false);
    Check(!threw && result == VK_SUCCESS && buffer == recycled && driver.lastPool == newPool,
          "observer must preserve native handle reuse in another pool when tracking allocation fails");
    VkCommandBufferBeginInfo begin {};
    Check(Through<PFN_vkBeginCommandBuffer>(1, 0)(buffer, &begin) == VK_SUCCESS,
          "observer must preserve successful Begin on an untracked allocation");
    const auto stale = R().Current(1, Key(buffer));
    if (stale)
    {
        auto incorrectlyAdmitted = Pin(*stale);
        Through<PFN_vkDestroyCommandPool>(6, 1)(device, pool, nullptr);
        Check(!incorrectlyAdmitted.expired(),
              "observer old-pool destruction must not retire a newly recorded buffer in another native pool");
    }
    Check(!stale, "observer must refuse recycled handle after an allocation notification was lost under OOM");
    Check(resource.expired(), "observer must retire the old allocation's pins despite OOM");

    Through<PFN_vkFreeCommandBuffers>(4, 0)(device, newPool, 1, &buffer);
    driver.nextBuffer = Key(recycled);
    Check(Through<PFN_vkAllocateCommandBuffers>(0, 0)(device, &info, &buffer) == VK_SUCCESS,
          "observer allocation must recover after OOM");
    auto recovered = Pin(BeginThrough(device, buffer, 1));
    Check(!R().Pin(old, { std::make_shared<int>(1) }), "observer must reject a ticket from the previous allocation");
    Through<PFN_vkDestroyCommandPool>(6, 0)(device, pool, nullptr);
    Check(!recovered.expired(), "observer old-pool destruction must preserve the recovered new-pool recording");
    Through<PFN_vkDestroyCommandPool>(6, 1)(device, newPool, nullptr);
    Check(recovered.expired(), "observer new-pool destruction must retire its actual recording");
}

void ObserverBulkFreePreservesReentrantAllocation()
{
    HookDevice(device);
    const std::array<VkCommandBuffer, 2> buffers { AllocateThrough(device, 1), AllocateThrough(device, 1) };
    const auto firstTicket = BeginThrough(device, buffers[0], 1);
    auto oldSecond = Pin(BeginThrough(device, buffers[1], 1));
    bool reentered = false;
    bool allDetached = false;
    std::weak_ptr<int> replacement;
    auto first = std::shared_ptr<int>(nullptr,
                                      [&](int*)
                                      {
                                          allDetached = !R().Current(Key(device), Key(buffers[0])) &&
                                                        !R().Current(Key(device), Key(buffers[1]));
                                          driver.nextBuffer = Key(buffers[1]);
                                          auto reused = AllocateThrough(device, 0);
                                          VkCommandBufferBeginInfo begin {};
                                          Through<PFN_vkBeginCommandBuffer>(1, 0)(reused, &begin);
                                          const auto ticket = R().Current(Key(device), Key(reused));
                                          auto token = std::make_shared<int>(3);
                                          replacement = token;
                                          reentered = reused == buffers[1] && ticket && R().Pin(*ticket, { token });
                                      });
    Check(R().Pin(firstTicket, { first }), "setup must pin the observer's reentrant first member");
    first.reset();
    Through<PFN_vkFreeCommandBuffers>(4, 1)(device, pool, buffers.size(), buffers.data());
    Check(reentered && !replacement.expired() && R().Current(Key(device), Key(buffers[1])),
          "bulk observer Free must preserve a handle recycled by its first release callback");
    Check(allDetached && oldSecond.expired() && calls[4][1] == 1,
          "bulk observer Free must forward once and remove all old metadata before callbacks");
    Through<PFN_vkResetCommandBuffer>(3, 0)(buffers[1], 0);
    Check(replacement.expired(), "observer reentrant allocation must survive until its own later reset");
}

int main()
{
    const std::pair<const char*, void (*)()> cases[] = {
        { "independent-export-procedure-coverage", InstallCoversExportAndProcedure },
        { "multiple-targets-aliases", MultipleTargetsAndAliases },
        { "observer-success-boundaries", ObserverSuccessBoundaries },
        { "observer-begin-flags-reach-ticket", ObserverBeginFlagsReachTheTicket },
        { "host-install-opt-in", InstallRemainsOptIn },
        { "detour-failure-own-address", FailedDetourAndOwnAddresses },
        { "partial-install-is-not-lifetime-proof", PartialInstallCannotCertifyRecordingLifetime },
        { "serial-secondary-is-not-primary-proof", SerialSecondaryIsNotPrimaryEvidence },
        { "per-device-coverage-certificates", CoverageCertificates },
        { "transaction-phase-failures", TransactionFailuresCannotCertify },
        { "target-capacity-refusal", TargetSaturationCannotCertify },
        { "observer-allocation-failure", ObserverAllocationFailure },
        { "observer-handle-reuse-after-oom", ObserverHandleReuseAfterOutOfMemory },
        { "observer-bulk-free-reentrant-reuse", ObserverBulkFreePreservesReentrantAllocation },
    };
    int failures = 0;
    for (const auto& [name, run] : cases)
    {
        FreshInstall();
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
    R().ForgetDevice(1);
    R().ForgetDevice(2);
    return failures == 0 ? 0 : 1;
}
