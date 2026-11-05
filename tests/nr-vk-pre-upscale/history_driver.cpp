#include "pch.h"
#include <dlssnr/DlssNr_VkPreScope.h>
#include <dlssnr/DlssNr_VkRecording.h>
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>
// Production admission and model-call slices are inserted below. Allocation/shader preparation is fake.
struct Option
{
    unsigned value = 1;
    unsigned value_or_default() const { return value; }
};
struct Config
{
    Option DlssNrEnabled, DlssNrIntensity, DlssNrStyle, DlssNrLocalStructure, DlssNrLocalTone, DlssNrSkinStructure,
        DlssNrAutoMask;
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};
struct NVSDK_NGX_Resource_VK
{
    struct
    {
        struct
        {
            uintptr_t Image = 1;
        } ImageViewInfo;
    } Resource;
};
struct NVSDK_NGX_Parameter
{
    void Get(const char*, void**) {}
};
constexpr const char* NVSDK_NGX_Parameter_Color = "Color";
template <class... T> void Log(T&&...) {}
#define LOG_WARN(...) Log(__VA_ARGS__)
#define LOG_INFO(...) Log(__VA_ARGS__)
void SessionGuardOff() {}
namespace DlssNr::Pictures
{
enum class Role
{
    Main,
    Aside
};
enum class Consumed
{
    Same,
    Stale,
    Restart
};
bool aside = false;
struct Decision
{
    Role role;
};
template <class F> Decision Ask(NVSDK_NGX_Parameter*, F) { return { aside ? Role::Aside : Role::Main }; }
int EvaluateEpoch(NVSDK_NGX_Parameter*) { return 1; }
struct EpochGate
{
    Consumed Consume(int) { return Consumed::Same; }
};
} // namespace DlssNr::Pictures
namespace DlssNr::SlNativeNr
{
int Instance() { return 0; }
int NowMs() { return 0; }
const char* AskInto(int, std::vector<std::string>&, int) { return nullptr; }
} // namespace DlssNr::SlNativeNr
namespace DlssNr::SlTagInventory
{
struct Inventory
{
    bool TakeLine(std::string&, int) { return false; }
};
Inventory Instance() { return {}; }
int NowMs() { return 0; }
} // namespace DlssNr::SlTagInventory
using namespace DlssNr;
struct Invalidatable
{
    void Invalidate() {}
    void Reset() {}
};
struct Image
{
    int ngx = 0;
};
struct State
{
    bool reset = false, failed = false;
    unsigned frames = 0, meterFrames = 0;
    float gameExposure = 0;
    void *feature = reinterpret_cast<void*>(7), *capabilityParams = nullptr;
    Image output;
    Invalidatable meterPending, exposureLatch;
    std::shared_ptr<void> recording = std::make_shared<int>(1);
} g_vk;
std::mutex g_vkMutex;
std::atomic<bool> g_vkResetOnReturn { false };
Pictures::EpochGate g_vkPictureEpoch;
VkPre::Placement g_nativePlacement;
std::atomic_flag g_preBorrow = ATOMIC_FLAG_INIT;
bool skipPreparation = false;
std::vector<int> modelResets;
struct Forwarder
{
    template <class... A> int evaluate(A... args)
    {
        const auto values = std::make_tuple(args...);
        assert(std::get<1>(values) == reinterpret_cast<void*>(7));       // same generation, no rebuild
        assert(std::get<7>(values) == 100 && std::get<8>(values) == 50); // equal render/output size
        modelResets.push_back(std::get<18>(values));                     // actual reset argument passed by production
        return 1;
    }
} g_fwd;
Image input;
Image* modelInput = &input;
void *depthForModel = nullptr, *motionForModel = nullptr;
unsigned workWidth = 100, workHeight = 50;
bool depthInverted = false;
struct Handed
{
    struct Region
    {
        unsigned width = 100, height = 50, baseX = 0, baseY = 0;
    } depth, motion;
    float scaleX = 1, scaleY = 1;
} handed;
// The native Vulkan pair capture the post body records into (DlssNr_PairCaptureVk.h); never requested here, so only
// its shape is needed for the production body to compile.
namespace PairCapture
{
struct AnyField
{
    template <class T> AnyField& operator=(const T&) { return *this; }
};
struct VkFacts
{
    AnyField route, surface, hookMethod, width, height, linearHdr, whitePoint, frame, present, settings;
};
struct VkFrame
{
    AnyField device, physical, cmd, image, view, range, format, recording, refusal;
    VkFacts facts;
};
inline bool WantedVk() { return false; }
inline void PollVkLocked() {}
inline void CancelVkLocked() {}
inline bool BeginVkLocked(VkFrame&&) { return false; }
template <class C> void CommitVkLocked(C) {}
} // namespace PairCapture
// PRODUCTION
int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string scenario = argv[1];
    auto& r = VkRecording::Recordings();
    r.Allocate(1, 10, { 20 });
    r.Begin(20, VkRecording::Reuse::Serial);
    const auto command = reinterpret_cast<VkCommandBuffer>(20);
    NVSDK_NGX_Parameter params;
    auto post = [&]
    {
        EvaluateAfterPreSafeVk(command, &params, reinterpret_cast<VkInstance>(1), reinterpret_cast<VkPhysicalDevice>(1),
                               reinterpret_cast<VkDevice>(1));
    };
    if (scenario == "post-only")
    {
        post();
        post();
        assert((modelResets == std::vector<int> { 0, 0 }));
        return 0;
    }
    auto pre = [&]
    {
        assert(g_nativePlacement.TryPre());
        std::lock_guard lock(g_vkMutex);
        const auto ticket = r.Current(1, 20);
        assert(ticket && r.Pin(*ticket, { g_vk.recording }));
        g_vk.reset = true; // admitted pre prepares a reset on every generation use
        PreModelCall(command);
    };
    pre();
    assert((modelResets == std::vector<int> { 1 }));
    post(); // still recording: must not enter post even though Current() is known
    assert(modelResets.size() == 1 && !g_vk.reset);
    r.End(20);
    post(); // End does not retire the generation; the wrapper must refuse it.
    assert(modelResets.size() == 1 && !g_vk.reset);
    r.Reset(20); // represents legal game retirement after completion; no GPU wait in the fixture
    assert(g_vk.recording.use_count() == 1);
    post(); // unknown recording: cannot consume the reset
    assert(modelResets.size() == 1 && !g_vk.reset);
    r.Begin(20, VkRecording::Reuse::Serial);
    if (scenario == "skipped-gap")
    {
        Pictures::aside = true;
        post();
        Pictures::aside = false;
        assert(modelResets.size() == 1 && !g_vk.reset); // other picture cannot mutate history
        skipPreparation = true;
        post();
        post();
        skipPreparation = false;
        assert(modelResets.size() == 1); // skipped after latch consumption must retain reset
    }
    else
        assert(scenario == "equal-size");
    post();
    post();
    assert((modelResets == std::vector<int> { 1, 1, 0 }));
    pre();
    r.End(20);
    r.Reset(20);
    r.Begin(20, VkRecording::Reuse::Serial);
    post();
    post();
    assert((modelResets == std::vector<int> { 1, 1, 0, 1, 1, 0 }));
}
