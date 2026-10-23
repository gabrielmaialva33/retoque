// Host fakes for the production Vulkan device-creation path: OptiScaler's hkvkCreateDevice
// (hooks/Vulkan_Hooks.cpp), VulkanSpoofing::hkvkCreateDevice and VulkanDeviceFeatureState
// (spoofing/Vulkan_Spoofing.cpp) and the NR extension block (retoque/dlssnr/DlssNr_VkExtensions.h).
// run.py appends the production slices and cases.cpp after this file.
//
// What is fake: Windows types, logging, Config and State, every collaborator the hook calls after the
// driver returned, and the driver itself. The fake driver answers feature and extension queries from a
// per-GPU table and, at vkCreateDevice, deep-copies exactly the request it was handed and checks it against
// the device-creation rules this suite is about. It runs no Vulkan.
#include <vulkan/vulkan.h>

// vulkan_win32.h's names, without windows.h
#define VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME "VK_KHR_external_memory_win32"
#define VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME "VK_KHR_external_semaphore_win32"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// ---- Windows ----------------------------------------------------------------------------------------------

using HMODULE = void*;
using HWND = void*;
using UINT = unsigned int;
using UINT64 = unsigned long long;
using FARPROC = void (*)();
struct LUID
{
    uint32_t LowPart;
    int32_t HighPart;
};
using PLUID = LUID*;
inline bool IsEqualLUID(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
#define _ReturnAddress() __builtin_return_address(0)

// ---- Logging: every argument is evaluated, nothing is printed ---------------------------------------------

template <typename... A> inline void LogSink(A&&...) {}
#define LOG_TRACE(...) LogSink(__VA_ARGS__)
#define LOG_DEBUG(...) LogSink(__VA_ARGS__)
#define LOG_INFO(...) LogSink(__VA_ARGS__)
#define LOG_WARN(...) LogSink(__VA_ARGS__)
#define LOG_ERROR(...) LogSink(__VA_ARGS__)
#define LOG_FUNC() ((void) 0)
#define LOG_FUNC_RESULT(x) LogSink(x)

// ---- Config and State -------------------------------------------------------------------------------------

template <typename T> struct FakeOption
{
    T value;
    T value_or_default() const { return value; }
};

struct Config
{
    FakeOption<bool> VulkanExtensionSpoofing { false };
    FakeOption<bool> DlssNrEnabled { false };
    FakeOption<bool> OverlayMenu { false };

    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};

struct State
{
    // @creatingD3DDevice@ (the production declaration from State.h, put here by run.py)
    bool vkAntiLagSupported = false;
    VkInstance VulkanInstance = VK_NULL_HANDLE;

    static State& Instance()
    {
        static State state;
        return state;
    }
};

struct ScopedSkipSpoofingGlobal
{
    ScopedSkipSpoofingGlobal() {}
    ~ScopedSkipSpoofingGlobal() {}
};

// ---- What the hook calls around the create, all inert -------------------------------------------------------

inline HMODULE vulkanModule = nullptr;

struct KernelBaseProxy
{
    using GetModuleHandleA_t = HMODULE (*)(const char*);
    using GetProcAddress_t = FARPROC (*)(HMODULE, const char*);
    static GetModuleHandleA_t GetModuleHandleA_()
    {
        return [](const char*) -> HMODULE { return nullptr; };
    }
    static GetProcAddress_t GetProcAddress_()
    {
        return [](HMODULE, const char*) -> FARPROC { return nullptr; };
    }
};

struct IFGFeature_Vk
{
    int prepared = 0;
    int created = 0;
    void PrepareDevice(VkPhysicalDevice, VkDeviceCreateInfo&) { ++prepared; }
    void OnDeviceCreated(VkDevice) { ++created; }
};

// The FG output the hook would create; nullptr (no FG) unless a case sets one for its thread.
inline thread_local IFGFeature_Vk* gFakeFG = nullptr;
static IFGFeature_Vk* CreateFGFeature() { return gFakeFG; }
static bool IsDxgiBackedCaller(void*) { return false; }
static void HookDevice(VkDevice) {}

namespace Hudfix_Vk
{
inline void HookDevice(VkDevice) {}
} // namespace Hudfix_Vk

namespace DeferredRelease
{
inline void RegisterOwner(uint64_t) {}
inline void DropOwnersExcept(uint64_t) {}
} // namespace DeferredRelease

// Records what MenuOverlayVk::DeviceCreated reads, which is only the queue create infos.
namespace MenuOverlayVk
{
inline void DeviceCreated(VkDevice, const VkDeviceCreateInfo* pCreateInfo)
{
    for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++)
        (void) pCreateInfo->pQueueCreateInfos[i].queueCount;
}
} // namespace MenuOverlayVk

struct PrimaryGpu
{
    LUID luid {};
};
namespace IdentifyGpu
{
inline PrimaryGpu getPrimaryGpu() { return {}; }
} // namespace IdentifyGpu

struct VulkanHooks
{
    inline static PFN_vkAntiLagUpdateAMD o_vkAntiLagUpdateAMD = nullptr;
    inline static PFN_vkSetLatencySleepModeNV o_vkSetLatencySleepModeNV = nullptr;
    inline static PFN_vkLatencySleepNV o_vkLatencySleepNV = nullptr;
    inline static PFN_vkSetLatencyMarkerNV o_vkSetLatencyMarkerNV = nullptr;
    inline static PFN_vkGetLatencyTimingsNV o_vkGetLatencyTimingsNV = nullptr;
    inline static PFN_vkQueueNotifyOutOfBandNV o_vkQueueNotifyOutOfBandNV = nullptr;
};

// ---- The fake driver ----------------------------------------------------------------------------------------

static void Fail(const std::string& message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

static void Check(bool condition, const std::string& message)
{
    if (!condition)
        Fail(message);
}

// Every feature struct the path reads or writes: header (sType, pNext) then VkBool32s only.
struct FeatureStructInfo
{
    VkStructureType sType;
    const char* name;
    size_t size;
    const char* extension; // the extension that provides it, nullptr for core-only structs
    uint32_t core;         // the version it is core in, 0 if never
};

#define FEATURE(type, stype, ext, core) { stype, #type, sizeof(type), ext, core }
static const FeatureStructInfo kFeatureStructs[] = {
    FEATURE(VkPhysicalDeviceFeatures2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, nullptr, VK_API_VERSION_1_0),
    FEATURE(VkPhysicalDeviceVulkan11Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, nullptr,
            VK_API_VERSION_1_2),
    FEATURE(VkPhysicalDeviceVulkan12Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, nullptr,
            VK_API_VERSION_1_2),
    FEATURE(VkPhysicalDeviceVulkan13Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, nullptr,
            VK_API_VERSION_1_3),
    FEATURE(VkPhysicalDeviceVulkan14Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, nullptr,
            VK_API_VERSION_1_4),
    FEATURE(VkPhysicalDeviceShaderFloat16Int8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
            VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_API_VERSION_1_2),
    FEATURE(VkPhysicalDevice8BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,
            VK_KHR_8BIT_STORAGE_EXTENSION_NAME, VK_API_VERSION_1_2),
    FEATURE(VkPhysicalDeviceDescriptorIndexingFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
            VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, VK_API_VERSION_1_2),
    FEATURE(VkPhysicalDeviceBufferDeviceAddressFeatures,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_API_VERSION_1_2),
    FEATURE(VkPhysicalDeviceBufferDeviceAddressFeaturesEXT,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT,
            VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, 0),
    FEATURE(VkPhysicalDeviceVulkanMemoryModelFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,
            VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME, VK_API_VERSION_1_2),
    FEATURE(VkPhysicalDeviceTimelineSemaphoreFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
            VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_API_VERSION_1_2),
    FEATURE(VkPhysicalDeviceSynchronization2Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
            VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_API_VERSION_1_3),
    FEATURE(VkPhysicalDeviceSubgroupSizeControlFeatures,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES,
            VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, VK_API_VERSION_1_3),
    FEATURE(VkPhysicalDeviceDescriptorBufferFeaturesEXT,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
            0),
    FEATURE(VkPhysicalDeviceShaderFloat8FeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT,
            VK_EXT_SHADER_FLOAT8_EXTENSION_NAME, 0),
    FEATURE(VkPhysicalDeviceCooperativeMatrixFeaturesKHR,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
            0),
    FEATURE(VkPhysicalDeviceCooperativeMatrix2FeaturesNV,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV,
            VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME, 0),
    FEATURE(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR,
            VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME, 0),
    FEATURE(VkPhysicalDeviceAntiLagFeaturesAMD, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD,
            VK_AMD_ANTI_LAG_EXTENSION_NAME, 0),
};
#undef FEATURE

static constexpr size_t kHeader = sizeof(VkBaseOutStructure);

static const FeatureStructInfo* FindInfo(VkStructureType sType)
{
    for (const auto& info : kFeatureStructs)
    {
        if (info.sType == sType)
            return &info;
    }
    return nullptr;
}

static size_t BitCount(const FeatureStructInfo& info) { return (info.size - kHeader) / sizeof(VkBool32); }

static VkBool32* Bits(void* structure) { return reinterpret_cast<VkBool32*>(static_cast<char*>(structure) + kHeader); }
static const VkBool32* Bits(const void* structure)
{
    return reinterpret_cast<const VkBool32*>(static_cast<const char*>(structure) + kHeader);
}

template <typename T> static size_t BitIndex(VkBool32 T::* member)
{
    static T probe {};
    return (reinterpret_cast<char*>(&(probe.*member)) - reinterpret_cast<char*>(&probe) - kHeader) / sizeof(VkBool32);
}

// One GPU as the fake driver presents it. The handle the path sees is a pointer to this.
struct FakeGpu
{
    std::string name;
    uint32_t apiVersion = VK_API_VERSION_1_3;
    std::vector<std::string> extensions;
    std::set<std::pair<VkStructureType, size_t>> unsupported; // feature bits the device lacks

    enum class Enumerate
    {
        Ok,
        CountFails,     // the count query fails
        DataFails,      // the count query works, the data query fails
        IncompleteOnce, // the last extension appears between the two queries: the first data query is VK_INCOMPLETE
    } enumerate = Enumerate::Ok;
    std::atomic<int> dataQueries { 0 };

    // onEntry runs at vkCreateDevice entry, before the driver reads anything; inCreate after it copied the request,
    // before it reads the request again and returns. A case uses them to hold one thread's create while another
    // thread creates, or to create again from inside the driver. inFeatures runs after a features query answered.
    std::function<void()> onEntry;
    std::function<void()> inCreate;
    std::function<void()> inFeatures;

    bool Has(const std::string& extension) const
    {
        return std::ranges::find(extensions, extension) != extensions.end();
    }

    template <typename T> void Lacks(VkStructureType sType, VkBool32 T::* member)
    {
        unsupported.insert({ sType, BitIndex(member) });
    }

    VkPhysicalDevice Handle() { return reinterpret_cast<VkPhysicalDevice>(this); }
};

static FakeGpu* Gpu(VkPhysicalDevice physicalDevice) { return reinterpret_cast<FakeGpu*>(physicalDevice); }

static uint32_t gInstanceApiVersion = VK_API_VERSION_1_3; // what the driver applies, known or not to the hook

static uint32_t EffectiveApi(const FakeGpu& gpu) { return std::min(gInstanceApiVersion, gpu.apiVersion); }

// Whether the device can report a feature struct at all at this effective version.
static bool StructAvailable(const FakeGpu& gpu, const FeatureStructInfo& info)
{
    if (info.core != 0 && EffectiveApi(gpu) >= info.core)
        return true;
    return info.extension != nullptr && gpu.Has(info.extension);
}

static bool BitSupported(const FakeGpu& gpu, const FeatureStructInfo& info, size_t bit)
{
    return StructAvailable(gpu, info) && !gpu.unsupported.contains({ info.sType, bit });
}

static VKAPI_ATTR void VKAPI_CALL FakeGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                                                 VkPhysicalDeviceFeatures2* pFeatures)
{
    auto& gpu = *Gpu(physicalDevice);
    for (auto* node = reinterpret_cast<VkBaseOutStructure*>(pFeatures); node != nullptr; node = node->pNext)
    {
        const auto* info = FindInfo(node->sType);
        if (info == nullptr)
            continue;
        for (size_t bit = 0; bit < BitCount(*info); bit++)
            Bits(node)[bit] = BitSupported(gpu, *info, bit) ? VK_TRUE : VK_FALSE;
    }

    if (gpu.inFeatures)
        gpu.inFeatures();
}

static VKAPI_ATTR VkResult VKAPI_CALL FakeEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                             const char* pLayerName,
                                                                             uint32_t* pPropertyCount,
                                                                             VkExtensionProperties* pProperties)
{
    auto& gpu = *Gpu(physicalDevice);
    Check(pLayerName == nullptr, "only the implementation's own extensions are asked for");

    // IncompleteOnce: the last extension of the list appears with the first data query, so a count taken
    // before it is one short and that first data query is VK_INCOMPLETE.
    std::vector<std::string> list = gpu.extensions;
    if (gpu.enumerate == FakeGpu::Enumerate::IncompleteOnce)
    {
        if (pProperties != nullptr)
            ++gpu.dataQueries;
        if (gpu.dataQueries == 0)
            list.pop_back();
    }

    if (gpu.enumerate == FakeGpu::Enumerate::CountFails)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    if (pProperties == nullptr)
    {
        *pPropertyCount = (uint32_t) list.size();
        return VK_SUCCESS;
    }

    if (gpu.enumerate == FakeGpu::Enumerate::DataFails)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    const uint32_t written = std::min<uint32_t>(*pPropertyCount, (uint32_t) list.size());
    for (uint32_t i = 0; i < written; i++)
    {
        pProperties[i] = {};
        std::snprintf(pProperties[i].extensionName, sizeof(pProperties[i].extensionName), "%s", list[i].c_str());
        pProperties[i].specVersion = 1;
    }
    *pPropertyCount = written;
    return written < list.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL FakeGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                                  VkPhysicalDeviceProperties* pProperties)
{
    *pProperties = {};
    pProperties->apiVersion = Gpu(physicalDevice)->apiVersion;
}

// What one vkCreateDevice call received, copied while the call was running.
struct Received
{
    FakeGpu* gpu = nullptr;
    std::vector<std::string> extensions;
    std::vector<std::pair<VkStructureType, std::vector<VkBool32>>> chain; // in order, known structs' bits
    std::vector<VkStructureType> chainTypes;                              // in order, every struct
    bool enabledFeaturesSet = false;
    VkPhysicalDeviceFeatures enabledFeatures {};
    uint32_t queueCreateInfoCount = 0;
    const VkDeviceQueueCreateInfo* pQueueCreateInfos = nullptr;
    std::vector<std::string> violations;
    std::vector<std::string> changedWhileRunning; // the request read again before returning, against the first copy

    bool Has(const std::string& extension) const
    {
        return std::ranges::find(extensions, extension) != extensions.end();
    }

    const std::vector<VkBool32>* Struct(VkStructureType sType) const
    {
        for (const auto& [type, bits] : chain)
        {
            if (type == sType)
                return &bits;
        }
        return nullptr;
    }

    template <typename T> std::optional<bool> Bit(VkStructureType sType, VkBool32 T::* member) const
    {
        const auto* bits = Struct(sType);
        if (bits == nullptr)
            return std::nullopt;
        return (*bits)[BitIndex(member)] == VK_TRUE;
    }
};

// A deep copy of a create info, for "the request as the driver saw it" and "the game's request".
static Received Copy(const VkDeviceCreateInfo* info)
{
    Received copy;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++)
        copy.extensions.emplace_back(info->ppEnabledExtensionNames[i]);
    for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node != nullptr; node = node->pNext)
    {
        copy.chainTypes.push_back(node->sType);
        if (const auto* known = FindInfo(node->sType))
            copy.chain.push_back({ node->sType, std::vector<VkBool32>(Bits(node), Bits(node) + BitCount(*known)) });
    }
    copy.enabledFeaturesSet = info->pEnabledFeatures != nullptr;
    if (info->pEnabledFeatures != nullptr)
        copy.enabledFeatures = *info->pEnabledFeatures;
    copy.queueCreateInfoCount = info->queueCreateInfoCount;
    copy.pQueueCreateInfos = info->pQueueCreateInfos;
    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++)
        (void) info->pQueueCreateInfos[i].pQueuePriorities[0];
    return copy;
}

static bool SameRequest(const Received& a, const Received& b)
{
    return a.extensions == b.extensions && a.chain == b.chain && a.chainTypes == b.chainTypes &&
           a.enabledFeaturesSet == b.enabledFeaturesSet &&
           std::memcmp(&a.enabledFeatures, &b.enabledFeatures, sizeof(a.enabledFeatures)) == 0 &&
           a.queueCreateInfoCount == b.queueCreateInfoCount && a.pQueueCreateInfos == b.pQueueCreateInfos;
}

// The rules a request is checked against. Each string names one broken rule; the cases then require that
// OptiScaler adds none to what the game itself asked.
static std::vector<std::string> Violations(const FakeGpu& gpu, const Received& request)
{
    std::vector<std::string> out;
    const auto has = [&](const char* name) { return request.Has(name); };
    const uint32_t api = EffectiveApi(gpu);

    for (const auto& extension : request.extensions)
    {
        if (!gpu.Has(extension))
            out.push_back("extension the device does not offer: " + extension);
    }

    std::set<std::string> unique(request.extensions.begin(), request.extensions.end());
    if (unique.size() != request.extensions.size())
        out.push_back("an extension listed twice");

    if (has(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) && has(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
        out.push_back("VK_KHR_ and VK_EXT_buffer_device_address together (03328)");

    std::set<VkStructureType> seen;
    for (auto sType : request.chainTypes)
    {
        if (!seen.insert(sType).second)
            out.push_back("a structure type twice in the chain (sType-unique): " + std::to_string((int) sType));
    }

    if (request.enabledFeaturesSet && seen.contains(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2))
        out.push_back("pEnabledFeatures beside VkPhysicalDeviceFeatures2 (00373)");

    if (request.enabledFeaturesSet)
    {
        const auto* bits = reinterpret_cast<const VkBool32*>(&request.enabledFeatures);
        for (size_t bit = 0; bit < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); bit++)
        {
            if (bits[bit] == VK_TRUE && gpu.unsupported.contains({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, bit }))
                out.push_back("an unsupported core feature in pEnabledFeatures: bit " + std::to_string(bit));
        }
    }

    for (const auto& [sType, bits] : request.chain)
    {
        const auto& info = *FindInfo(sType);
        const bool any = std::ranges::any_of(bits, [](VkBool32 b) { return b == VK_TRUE; });
        if (info.extension == nullptr && api < info.core)
            out.push_back(std::string(info.name) + " on a device below its core version");
        else if (any && !(info.core != 0 && api >= info.core) && info.extension != nullptr && !has(info.extension))
            out.push_back(std::string(info.name) + " enabled without " + info.extension);

        for (size_t bit = 0; bit < bits.size(); bit++)
        {
            if (bits[bit] == VK_TRUE && !BitSupported(gpu, info, bit))
                out.push_back(std::string("an unsupported feature: ") + info.name + " bit " + std::to_string(bit));
        }
    }

    const auto pairs = [&](VkStructureType aggregate, std::initializer_list<VkStructureType> promoted, const char* vuid)
    {
        if (!seen.contains(aggregate))
            return;
        for (auto sType : promoted)
        {
            if (seen.contains(sType))
                out.push_back(std::string("a promoted struct beside its VkPhysicalDeviceVulkanNNFeatures (") + vuid +
                              "): " + FindInfo(sType)->name);
        }
    };
    pairs(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
          { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES },
          "02830");
    pairs(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
          { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES },
          "06532");

    if (has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
    {
        if (auto push = request.Bit(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
                                    &VkPhysicalDeviceVulkan14Features::pushDescriptor);
            push.has_value() && !*push)
            out.push_back("VK_KHR_push_descriptor with VkPhysicalDeviceVulkan14Features::pushDescriptor off");
    }

    const auto deviceScopeAlone = [&](VkStructureType sType, auto model, auto scope)
    {
        auto m = request.Bit(sType, model);
        auto s = request.Bit(sType, scope);
        if (s.value_or(false) && !m.value_or(false))
            out.push_back("vulkanMemoryModelDeviceScope without vulkanMemoryModel");
    };
    deviceScopeAlone(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                     &VkPhysicalDeviceVulkan12Features::vulkanMemoryModel,
                     &VkPhysicalDeviceVulkan12Features::vulkanMemoryModelDeviceScope);
    deviceScopeAlone(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,
                     &VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModel,
                     &VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModelDeviceScope);

    const bool coreBda = request
                             .Bit(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                  &VkPhysicalDeviceVulkan12Features::bufferDeviceAddress)
                             .value_or(false) ||
                         request
                             .Bit(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
                                  &VkPhysicalDeviceBufferDeviceAddressFeatures::bufferDeviceAddress)
                             .value_or(false);
    if (coreBda && has(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
        out.push_back("bufferDeviceAddress (core/KHR) on beside VK_EXT_buffer_device_address (04748)");

    return out;
}

struct FakeDevice
{
    Received received;
};

static std::mutex gDevicesMutex;
static std::vector<std::unique_ptr<FakeDevice>> gDevices;
static std::atomic<int> gCreates { 0 };

static VKAPI_ATTR VkResult VKAPI_CALL FakeCreateDevice(VkPhysicalDevice physicalDevice,
                                                       const VkDeviceCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks*, VkDevice* pDevice)
{
    auto& gpu = *Gpu(physicalDevice);
    if (gpu.onEntry)
        gpu.onEntry();

    auto device = std::make_unique<FakeDevice>();
    device->received = Copy(pCreateInfo);
    device->received.gpu = &gpu;
    device->received.violations = Violations(gpu, device->received);

    if (gpu.inCreate)
        gpu.inCreate();

    // Every pointer the request holds is read again just before returning: storage freed or reused while the
    // driver ran shows up here, as a difference or as an ASan report.
    const auto again = Copy(pCreateInfo);
    if (!SameRequest(again, device->received))
        device->received.changedWhileRunning = again.extensions;

    ++gCreates;
    std::scoped_lock lock(gDevicesMutex);
    *pDevice = reinterpret_cast<VkDevice>(device.get());
    gDevices.push_back(std::move(device));
    return VK_SUCCESS;
}

static const Received& ReceivedBy(VkDevice device)
{
    std::scoped_lock lock(gDevicesMutex);
    return reinterpret_cast<FakeDevice*>(device)->received;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FakeGetInstanceProcAddr(VkInstance, const char* pName)
{
    if (std::strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&FakeEnumerateDeviceExtensionProperties);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&FakeGetPhysicalDeviceFeatures2);
    return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FakeGetDeviceProcAddr(VkDevice, const char*) { return nullptr; }

// The loader's exports, which the production code falls back to.
extern "C"
{
    VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                        const char* pLayerName,
                                                                        uint32_t* pPropertyCount,
                                                                        VkExtensionProperties* pProperties)
    {
        return FakeEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                             VkPhysicalDeviceProperties* pProperties)
    {
        FakeGetPhysicalDeviceProperties(physicalDevice, pProperties);
    }

    VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                                              VkPhysicalDeviceProperties2* pProperties)
    {
        FakeGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);
    }
}
