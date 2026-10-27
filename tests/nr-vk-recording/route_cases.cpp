#include <vulkan/vulkan.h>
#include "SysUtils.h"
#include <dlssnr/DlssNr_VkRecording.h>
#include <set>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace VkRecording = DlssNr::VkRecording;
constexpr uint32_t kMeterSlots = 4;

template <class T> T H(uint64_t value) { return (T) (uintptr_t) value; }
void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
struct Counts
{
    unsigned feature = 0, images = 0, retired = 0, pass = 0, parameters = 0, query = 0, meter = 0, guide = 0, owner = 0,
             session = 0;
    unsigned waits = 0, deviceGone = 0;
    unsigned Freed() const { return feature + images + retired + pass + parameters + query + meter + guide; }
} g_counts;
struct OwnedImage
{
    unsigned id = 0;
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
struct LayoutEvent
{
    uint64_t command;
    VkImage image;
    VkImageLayout before, after;
};
std::vector<LayoutEvent> g_routeLayouts;
void DestroyImage(OwnedImage& image)
{
    if (image.id)
    {
        ++g_counts.images;
        image = OwnedImage {};
    }
}
void RetireImage(OwnedImage& image)
{
    if (image.id)
    {
        ++g_counts.retired;
        image = OwnedImage {};
    }
}
void DestroyMeterReadback() { ++g_counts.meter; }
struct Pass;
std::set<Pass*> g_livePasses;
std::set<void*> g_liveFeatures, g_liveParameters;
struct Pass
{
    Pass() { g_livePasses.insert(this); }
    ~Pass()
    {
        g_livePasses.erase(this);
        ++g_counts.pass;
    }
};
struct Params
{
};
void NVSDK_NGX_VULKAN_DestroyParameters(Params* parameters)
{
    ++g_counts.parameters;
    g_liveParameters.erase(parameters);
    delete parameters;
}
namespace DeferredRelease
{
std::vector<std::function<void()>> pending;
void Enqueue(std::function<void()> release, uint64_t) { pending.push_back(std::move(release)); }
void Drain()
{
    auto releases = std::move(pending);
    pending.clear();
    for (auto& release : releases)
        release();
}
} // namespace DeferredRelease
struct Guide
{
    bool ready = true;
    bool Ready() const { return ready; }
    VkDevice Device() const { return H<VkDevice>(1); }
    void Destroy() { ++g_counts.guide; }
    void Abandon() { ready = false; }
};
struct Exposure
{
    void Reset() {}
};
enum class Scaler
{
    Count
};
// Retirement ring behavior is covered by nr-vk-meter-readback; this boundary records invalidation.
struct MeterPending
{
    unsigned abandoned = 0;
    void Abandon() { ++abandoned; }
};
struct State
{
    VkDevice device = VK_NULL_HANDLE;
    void* feature = nullptr;
    Params* capabilityParams = nullptr;
    OwnedImage output, proxy, proxySmall, outputNative, keep, meter, depthMatched, motionMatched, depthRetired,
        motionRetired;
    Guide guideMatch;
    std::string guideMatchDeviceFailure, guideMatchSizeFailure, guideMatchTransient;
    unsigned guideMatchFailures = 0;
    std::unique_ptr<Pass> pass, superUp, superDown;
    Scaler nrScaler = Scaler::Count;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    uint64_t timedFrames = 0, meterFrames = 0;
    MeterPending meterPending;
    uint64_t meterAcceptedSerial = 0;
    Exposure exposureLatch;
    std::optional<double> lastGpuTime;
    unsigned width = 0, height = 0, workWidth = 0, workHeight = 0;
    VkBuffer meterReadback[4] {};
    VkDeviceMemory meterReadbackMemory[4] {};
    void* meterMapped[4] {};
    bool ngxInitialised = false, floatSlotKnown = false, reset = false;
    std::shared_ptr<void> recording;
} g_vk;
void ReleaseFeature(void* feature)
{
    ++g_counts.feature;
    g_liveFeatures.erase(feature);
    delete static_cast<int*>(feature);
}
struct Forwarder
{
    void (*release)(void*) = ReleaseFeature;
} g_fwd;
struct VkModelOwner
{
    enum class HandBack
    {
        Cancelled,
        NotRetiring,
        Done
    };
    HandBack FinishHandBack(uint64_t)
    {
        ++g_counts.owner;
        return HandBack::Done;
    }
    void DeviceGone(uint64_t) { ++g_counts.deviceGone; }
} g_owner;
enum class AfterUpscaleHandBack
{
    Busy,
    Cancelled,
    NotRetiring,
    Done
};
std::mutex g_vkMutex, g_ownerMutex;
void SessionGuardOff() { ++g_counts.session; }
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice, VkQueryPool, const VkAllocationCallbacks*)
{
    ++g_counts.query;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice)
{
    ++g_counts.waits;
    return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer command, VkPipelineStageFlags,
                                                           VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                                                           const VkMemoryBarrier*, uint32_t,
                                                           const VkBufferMemoryBarrier*, uint32_t count,
                                                           const VkImageMemoryBarrier* barriers)
{
    for (uint32_t index = 0; index < count; ++index)
        g_routeLayouts.push_back({ (uint64_t) (uintptr_t) command, barriers[index].image, barriers[index].oldLayout,
                                   barriers[index].newLayout });
}

// PRODUCTION_FUNCTIONS

struct Cleanup
{
    ~Cleanup()
    {
        g_vk.recording.reset();
        FreeRouteObjects();
        DeferredRelease::Drain();
    }
};
void Seed()
{
    g_vk = State {};
    g_vk.device = H<VkDevice>(1);
    g_vk.feature = new int(1);
    g_liveFeatures.insert(g_vk.feature);
    g_vk.capabilityParams = new Params;
    g_liveParameters.insert(g_vk.capabilityParams);
    g_vk.queryPool = H<VkQueryPool>(9);
    g_vk.output.id = 1;
    g_vk.proxy.id = 2;
    g_vk.proxySmall.id = 3;
    g_vk.outputNative.id = 4;
    g_vk.keep.id = 5;
    g_vk.meter.id = 6;
    g_vk.depthMatched.id = 7;
    g_vk.motionMatched.id = 8;
    g_vk.depthRetired.id = 9;
    g_vk.motionRetired.id = 10;
    g_vk.pass = std::make_unique<Pass>();
    g_vk.superUp = std::make_unique<Pass>();
    g_vk.superDown = std::make_unique<Pass>();
    g_vk.recording = std::make_shared<int>(42);
    g_vk.width = 1920;
    g_vk.height = 1080;
    unsigned image = 100;
    for (auto* owned : { &g_vk.proxy, &g_vk.keep, &g_vk.proxySmall, &g_vk.output, &g_vk.outputNative, &g_vk.meter,
                         &g_vk.depthMatched, &g_vk.motionMatched })
        owned->image = H<VkImage>(++image);
    g_counts = {};
    g_routeLayouts.clear();
}
void CheckAlive(void* feature, Pass* pass)
{
    Check(g_counts.Freed() == 0 && DeferredRelease::pending.empty(),
          "recorded generation was destroyed or scheduled for destruction while still referenced");
    Check(g_vk.feature == feature && g_vk.pass.get() == pass && g_vk.device == H<VkDevice>(1),
          "busy generation must preserve its feature, pass and device");
}
void FreeRetainsEveryRecording()
{
    Seed();
    Cleanup cleanup;
    auto recordingA = g_vk.recording;
    auto recordingB = g_vk.recording;
    void* feature = g_vk.feature;
    Pass* pass = g_vk.pass.get();
    FreeRouteObjects();
    CheckAlive(feature, pass);
    recordingA.reset();
    FreeRouteObjects();
    CheckAlive(feature, pass);
    recordingB.reset();
    FreeRouteObjects();
    DeferredRelease::Drain();
    Check(g_counts.feature == 1 && g_counts.images == 6 && g_counts.retired == 4 && g_counts.pass == 3 &&
              g_counts.parameters == 1 && g_counts.query == 1 && g_counts.guide == 1,
          "last recording retirement must allow real route teardown exactly once");
    Check(g_vk.feature == nullptr && !g_vk.pass && g_vk.device == VK_NULL_HANDLE,
          "unreferenced route must finish teardown");
}
void HandBackMustNotFinishOwnerEarly()
{
    Seed();
    Cleanup cleanup;
    auto recordingA = g_vk.recording;
    auto recordingB = g_vk.recording;
    void* feature = g_vk.feature;
    Pass* pass = g_vk.pass.get();
    std::vector<std::string> lines;
    Check(FinishAfterUpscaleHandBack(H<VkDevice>(1), lines) == AfterUpscaleHandBack::Busy,
          "handoff must remain Busy while two recordings retain the generation");
    Check(g_counts.owner == 0, "busy recording must be checked before owner hand-back is committed");
    CheckAlive(feature, pass);
    recordingA.reset();
    Check(FinishAfterUpscaleHandBack(H<VkDevice>(1), lines) == AfterUpscaleHandBack::Busy,
          "one recording reset must not unblock a generation retained by another");
    Check(g_counts.owner == 0, "remaining recording must preserve owner hand-back state");
    CheckAlive(feature, pass);
    recordingB.reset();
    Check(FinishAfterUpscaleHandBack(H<VkDevice>(1), lines) == AfterUpscaleHandBack::Done,
          "last recording retirement must permit hand-back");
    DeferredRelease::Drain();
    Check(g_counts.owner == 1 && g_counts.feature == 1 && g_counts.session == 1,
          "unblocked hand-back must commit owner and free its feature exactly once");
}
// Production intentionally releases CPU owners without running GPU destructors
// on abandonment. Reclaim only fake allocations after assertions, so LSan does
// not mistake this test's deliberately abandoned stub objects for a harness leak.
struct ReclaimAbandonedStubAllocations
{
    void* feature = g_vk.feature;
    Params* parameters = g_vk.capabilityParams;
    Pass* passes[3] = { g_vk.pass.get(), g_vk.superUp.get(), g_vk.superDown.get() };
    ~ReclaimAbandonedStubAllocations()
    {
        if (g_liveFeatures.erase(feature))
        {
            if (g_vk.feature == feature)
                g_vk.feature = nullptr;
            delete static_cast<int*>(feature);
        }
        if (g_liveParameters.erase(parameters))
        {
            if (g_vk.capabilityParams == parameters)
                g_vk.capabilityParams = nullptr;
            delete parameters;
        }
        for (auto* pass : passes)
            if (g_livePasses.count(pass))
            {
                if (g_vk.pass.get() == pass)
                    (void) g_vk.pass.release();
                if (g_vk.superUp.get() == pass)
                    (void) g_vk.superUp.release();
                if (g_vk.superDown.get() == pass)
                    (void) g_vk.superDown.release();
                delete pass;
            }
    }
};
void AbandonedDevice(bool throughDeviceGone)
{
    Seed();
    Cleanup cleanup;
    ReclaimAbandonedStubAllocations reclaim;
    auto& registry = VkRecording::Recordings();
    registry.ForgetDevice(1);
    registry.ForgetDevice(2);
    registry.Allocate(1, 5, { 11, 12 });
    registry.Begin(11);
    registry.Begin(12);
    registry.Allocate(2, 6, { 21 });
    registry.Begin(21);
    const auto first = registry.Current(1, 11), second = registry.Current(1, 12);
    Check(first.has_value() && second.has_value(), "test requires two observed open recordings");
    VkRecording::SlotPool slots(2);
    auto lease = slots.Acquire(2);
    std::weak_ptr<void> generation = g_vk.recording;
    Check(lease && registry.Pin(*first, { g_vk.recording, lease }) && registry.Pin(*second, { g_vk.recording, lease }),
          "both recordings must pin the route generation and its lease");
    lease.reset();
    Check(!slots.Acquire(1), "recording lease must remain busy before abandonment");
    if (throughDeviceGone)
    {
        std::vector<std::string> lines;
        AfterUpscaleDeviceGone(H<VkDevice>(1), true, lines);
        Check(g_counts.deviceGone == 1 && !lines.empty(), "device-gone path must notify the owner");
    }
    else
        ShutdownVk(false);
    Check(g_counts.Freed() == 0 && g_counts.waits == 0 && DeferredRelease::pending.empty(),
          "abandoned device must not invoke GPU release/destroy/wait or enqueue deferred GPU callbacks");
    Check(g_vk.device == VK_NULL_HANDLE && g_vk.feature == nullptr && g_vk.capabilityParams == nullptr && !g_vk.pass &&
              !g_vk.superUp && !g_vk.superDown,
          "abandonment must forget route handles without running their GPU destructors");
    Check(generation.expired(), "abandonment must release the old generation's recording pins");
    Check(!registry.Current(1, 11) && !registry.Current(1, 12) && registry.Current(2, 21).has_value(),
          "abandonment must invalidate only this device's recording registry entries");
    Check(!registry.Pin(*first, { std::make_shared<int>(7) }), "pre-abandonment ticket must not accept a new pin");
    Check(slots.Acquire(2) != nullptr, "forgetting the device must retire CPU-only slot leases");
    // Late lifecycle notifications must not resurrect entries or invoke a GPU callback.
    registry.Begin(11);
    Check(!registry.Current(1, 11), "late Begin must not resurrect an allocation forgotten with its device");
    registry.End(11);
    registry.Reset(11);
    registry.Free(1, 5, 11);
    registry.DestroyPool(1, 5);
    registry.ForgetDevice(1);
    Check(!registry.Current(1, 11) && g_counts.Freed() == 0 && g_counts.waits == 0 && DeferredRelease::pending.empty(),
          "late callbacks after abandonment must remain CPU-only and inadmissible");
    registry.ForgetDevice(2);
}
void OwnImageLayoutAfterDiscard()
{
    Seed();
    Cleanup cleanup;
    BeginRouteRecording();
    for (auto* image : { &g_vk.proxy, &g_vk.keep, &g_vk.proxySmall, &g_vk.output, &g_vk.outputNative, &g_vk.meter,
                         &g_vk.depthMatched, &g_vk.motionMatched })
    {
        Transition(H<VkCommandBuffer>(11), *image, VK_IMAGE_LAYOUT_GENERAL);
        Transition(H<VkCommandBuffer>(11), *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    Check(g_routeLayouts.size() == 16, "first route recording must establish and change all owned image layouts");
    // Recording A was discarded before submission: none of its transitions ran.
    g_routeLayouts.clear();
    BeginRouteRecording();
    for (auto* image : { &g_vk.proxy, &g_vk.keep, &g_vk.proxySmall, &g_vk.output, &g_vk.outputNative, &g_vk.meter,
                         &g_vk.depthMatched, &g_vk.motionMatched })
        Transition(H<VkCommandBuffer>(12), *image, VK_IMAGE_LAYOUT_GENERAL);
    Check(g_routeLayouts.size() == 8, "second recording must independently establish every rewritten image layout");
    for (const auto& event : g_routeLayouts)
        Check(event.command == 12 && event.before == VK_IMAGE_LAYOUT_UNDEFINED &&
                  event.after == VK_IMAGE_LAYOUT_GENERAL,
              "discarded route recording must not supply a cached oldLayout for the next recording");
}
int main()
{
    int failed = 0;
    const std::pair<const char*, void (*)()> cases[] = {
        { "v02-route-free-retains-two-recordings", FreeRetainsEveryRecording },
        { "v02-handoff-defers-owner-until-last-recording", HandBackMustNotFinishOwnerEarly },
        { "v02-owned-image-layout-after-recording-discard", OwnImageLayoutAfterDiscard },
        { "v02-shutdown-abandon-invalidates-without-gpu-callbacks", [] { AbandonedDevice(false); } },
        { "v02-device-gone-abandon-invalidates-without-gpu-callbacks", [] { AbandonedDevice(true); } },
    };
    for (auto [name, run] : cases)
        try
        {
            run();
            std::cout << "PASS: " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failed;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    return failed ? 1 : 0;
}
