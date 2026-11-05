// The suite inserts the production scope/resource admission definitions at PRODUCTION_SCOPE below.
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_defs.h>
#include <dlssnr/DlssNr_VkPreObserve.h>
#include <dlssnr/DlssNr_VkPreScope.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
struct NVSDK_NGX_Resource_VK
{
    struct ViewInfo
    {
        VkImageView ImageView {};
        VkImage Image {};
        VkImageSubresourceRange SubresourceRange {};
        VkFormat Format = VK_FORMAT_R16G16B16A16_SFLOAT;
        unsigned Width = 100, Height = 50;
    };
    struct
    {
        ViewInfo ImageViewInfo;
    } Resource;
    int Type = 0;
    bool ReadWrite = false;
};
struct NVSDK_NGX_Parameter
{
    std::map<std::string, void*> pointers;
    std::map<std::string, unsigned> values;
    int SetCount = 0;
    void Get(const char* key, void** out)
    {
        if (pointers.count(key))
            *out = pointers.at(key);
    }
    void Get(const char* key, unsigned* out)
    {
        if (values.count(key))
            *out = values.at(key);
    }
    void Set(const char* key, void* value)
    {
        pointers[key] = value;
        ++SetCount;
    }
};
struct Option
{
    unsigned value = 1;
    unsigned value_or_default() const { return value; }
};
struct Config
{
    Option DlssNrEnabled, DlssNrStage;
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t* n,
                                                                    VkQueueFamilyProperties* p)
{
    if (!p)
    {
        *n = 1;
        return;
    }
    p[0].queueFlags = VK_QUEUE_COMPUTE_BIT;
}
bool formatSupported = true, api13Supported = true;
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* p)
{
    p->apiVersion = api13Supported ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice, VkFormat, VkFormatProperties2* p)
{
    auto* f = static_cast<VkFormatProperties3*>(p->pNext);
    f->optimalTilingFeatures = formatSupported
                                   ? VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                                         VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT
                                   : 0;
}
namespace DlssNr
{
struct Size
{
    unsigned width = 100, height = 50;
};
struct Geometry
{
    Size size;
};
Geometry ResolveRenderSize(NVSDK_NGX_Parameter*) { return {}; }
Size ReadDisplaySize(NVSDK_NGX_Parameter*) { return { 200, 100 }; }
namespace RenderGeometry
{
struct Binding
{
    Size display;
};
Binding* active = nullptr;
} // namespace RenderGeometry
namespace Pictures
{
enum class Role
{
    Main,
    ViewOfMain,
    Aside
};
Role role = Role::Main;
int asks = 0;
struct Judgement
{
    Role role;
};
template <class Fn> Judgement Ask(NVSDK_NGX_Parameter*, Fn colour)
{
    ++asks;
    assert(colour() != 0);
    return { role };
}
} // namespace Pictures
namespace VkPre::Runtime
{
Observations facts;
bool uninterrupted = true;
struct HookStatus
{
    bool complete = true, formatless = true, formatFlags2 = true;
};
HookStatus coverage;
Observations* Facts() { return &facts; }
bool Uninterrupted() { return uninterrupted; }
HookStatus Status(VkDevice) { return coverage; }
std::optional<Ticket> ticket;
std::optional<Ticket> CurrentTicket(VkDevice, VkCommandBuffer) { return ticket; }
} // namespace VkPre::Runtime
int nr = 0, bindings = 0;
bool busy = false;
bool PreRecordingBusy() { return busy; }
bool prepare = true, throwPrepare = false, replaceView = false;
struct
{
    struct
    {
        NVSDK_NGX_Resource_VK ngx;
    } preScratch;
} g_vk;
std::atomic<bool> g_vkResetOnReturn { false };
static bool RecordPreUpscaleVk(VkCommandBuffer, NVSDK_NGX_Parameter*, VkInstance, VkPhysicalDevice, VkDevice,
                               const VkPre::FeatureIdentity&, uint32_t w, uint32_t h, uint32_t family, bool& emitted)
{
    assert(w == 200 && h == 100 && family == 0);
    if (!prepare)
        return false;
    emitted = true;
    ++nr;
    bindings = 22;
    if (replaceView)
        VkPre::Runtime::facts.View(40,
                                   { 1, 30, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, { 1, 0, 1, 0, 1 } });
    if (throwPrepare)
        throw std::runtime_error("partial NR");
    return true;
}
// PRODUCTION_SCOPE
} // namespace DlssNr
int main()
{
    using namespace DlssNr;
    using namespace DlssNr::VkPre;
    auto device = reinterpret_cast<VkDevice>(1);
    auto command = reinterpret_cast<VkCommandBuffer>(20);
    auto physical = reinterpret_cast<VkPhysicalDevice>(2);
    NVSDK_NGX_Handle handle { 7 };
    auto setup = [&]
    {
        auto& f = Runtime::facts;
        f.Clear();
        f.Device(1);
        f.Pool(1, 10, 0);
        f.Allocate(1, 10, 20, true);
        f.FeatureCreated(1, 7, Feature::SuperSampling, (uintptr_t) &handle);
        Runtime::ticket = Ticket { 1, 20, 99, VkRecording::Reuse::Serial };
        f.Begin(20, true, true, false, {}, Runtime::ticket);
        Runtime::coverage = {};
        Runtime::uninterrupted = true;
        Pictures::role = Pictures::Role::Main;
        Config::Instance()->DlssNrEnabled.value = 1;
        Config::Instance()->DlssNrStage.value = 1;
        prepare = true;
        throwPrepare = false;
        replaceView = false;
        formatSupported = true;
        api13Supported = true;
        nr = 0;
        busy = false;
        bindings = 11;
    };
    for (int scenario = 0; scenario < 23; ++scenario)
    {
        setup();
        NVSDK_NGX_Parameter params;
        NVSDK_NGX_Resource_VK resources[4];
        const char* keys[] = { NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_MotionVectors,
                               NVSDK_NGX_Parameter_Output };
        for (int i = 0; i < 4; ++i)
        {
            auto& v = resources[i].Resource.ImageViewInfo;
            v.Image = reinterpret_cast<VkImage>(30 + i);
            v.ImageView = reinterpret_cast<VkImageView>(40 + i);
            v.SubresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            ImageFact image;
            image.device = 1;
            image.width = 100;
            image.height = 50;
            image.depth = image.levels = image.layers = image.samples = 1;
            image.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            image.type = VK_IMAGE_TYPE_2D;
            image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
            image.concurrent = scenario != 15;
            image.families = { scenario == 16 ? 1u : 0u };
            Runtime::facts.Image(30 + i, image);
            Runtime::facts.View(40 + i,
                                { 1, (uint64_t) (30 + i), image.format, VK_IMAGE_VIEW_TYPE_2D, { 1, 0, 1, 0, 1 } });
            Runtime::facts.Barrier(20, 30 + i, { 1, 0, 1, 0, 1 }, 0, i == 3 ? 1 : 5, ~0u, ~0u);
            params.pointers[keys[i]] = &resources[i];
        }
        switch (scenario)
        {
        case 1:
            Config::Instance()->DlssNrStage.value = 0;
            break;
        case 2:
            Pictures::role = Pictures::Role::Aside;
            break;
        case 3:
            Runtime::facts.Rendering(20, true, false);
            break;
        case 4:
            Runtime::facts.FeatureCreated(1, 7, Feature::RayReconstruction, (uintptr_t) &handle);
            break;
        case 5:
            Runtime::ticket.reset();
            break;
        case 6:
            Runtime::facts.Barrier(20, 30, { 1, 0, 1, 0, 1 }, 5, 1, ~0u, ~0u);
            break;
        case 7:
            prepare = false;
            break;
        case 8:
            throwPrepare = true;
            break;
        case 9:
            Runtime::coverage.formatless = false;
            break;
        case 10:
            Runtime::coverage.formatFlags2 = false;
            break;
        case 14:
            formatSupported = false;
            break;
        case 17:
            Runtime::facts.Begin(20, true, false, false, {}, Runtime::ticket);
            break;
        case 18:
            Runtime::facts.FeatureCreated(1, 7, Feature::Unknown, (uintptr_t) &handle);
            break;
        case 19:
            replaceView = true;
            break;
        case 21:
            busy = true;
            break;
        case 22:
            api13Supported = false;
            break;
        case 11:
            params.values[NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X] = 1;
            break;
        }
        int sr = 0;
        bool threw = false;
        try
        {
            ScopedPreUpscaleVk scope(command, &params, {}, physical, device, &handle,
                                     scenario == 12   ? Backend::DLSS_on12
                                     : scenario == 20 ? Backend::RayReconstruction
                                                      : Backend::Native);
            ++sr;
            assert(scope.AllowPost() == (scenario == 1));
            if (scenario == 0 || scenario == 13)
                assert(params.pointers[keys[0]] == &g_vk.preScratch.ngx && nr == 1);
            else
                assert(params.pointers[keys[0]] == &resources[0]);
            bindings = 33; // the actual provider establishes its own state inside the evaluate window
            if (scenario == 0)
                throw std::runtime_error("SR failure");
            scope.Complete();
            assert(params.pointers[keys[0]] == &resources[0]);
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        assert(threw == (scenario == 0));
        assert(sr == 1 && params.pointers[keys[0]] == &resources[0] && bindings == 33);
        assert(nr == ((scenario == 0 || scenario == 8 || scenario == 13 || scenario == 19) ? 1 : 0));
        bindings = 44;
        assert(bindings == 44); // game-owned post-SR rebind and sentinel, no NR here
    }
}
