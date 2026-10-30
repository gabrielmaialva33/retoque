#include <cassert>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

using NVSDK_NGX_Result = unsigned;
constexpr unsigned NVSDK_NGX_Result_Success = 1, NVSDK_NGX_Result_Fail = 2;
constexpr unsigned NVSDK_NGX_Result_FAIL_InvalidParameter = 3, NVSDK_NGX_Result_FAIL_FeatureNotFound = 4;
constexpr unsigned NVNGX_PROVIDER_ID_OFFSET = 100;
struct NVSDK_NGX_Handle
{
    unsigned Id;
};
struct ID3D12GraphicsCommandList
{
};
struct NVSDK_NGX_Parameter
{
};
using NVSDK_NGX_Feature = int;
using VkDevice = void*;
using VkCommandBuffer = void*;
static int held = 0, living = 0;
struct InspectMutex
{
    bool locked = false;
    InspectMutex() { ++living; }
    void lock()
    {
        assert(!locked);
        locked = true;
        ++held;
    }
    void unlock()
    {
        assert(locked);
        locked = false;
        --held;
    }
    ~InspectMutex()
    {
        assert(!locked && "mutex destroyed while locked");
        --living;
    }
};
struct Provider
{
    unsigned createResult = 1, releaseResult = 1, releases = 0;
    NVSDK_NGX_Handle native { 42 };
    unsigned D3D12_CreateFeature(ID3D12GraphicsCommandList*, int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** out)
    {
        *out = &native;
        return createResult;
    }
    unsigned VULKAN_CreateFeature(VkCommandBuffer, int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** out)
    {
        *out = &native;
        return createResult;
    }
    unsigned VULKAN_CreateFeature1(VkDevice, VkCommandBuffer, int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** out)
    {
        *out = &native;
        return createResult;
    }
    unsigned D3D12_ReleaseFeature(NVSDK_NGX_Handle* in)
    {
        assert(in == &native && held == 1);
        ++releases;
        return releaseResult;
    }
    unsigned VULKAN_ReleaseFeature(NVSDK_NGX_Handle* in)
    {
        assert(in == &native && held == 1);
        ++releases;
        return releaseResult;
    }
};
Provider provider;
Provider* activeProvider = &provider;
class Nvngx_FG
{
  public:
    // @handle@
    static inline std::atomic_uint32_t lastIdCreated = 0;
    static Provider* getProvider() { return activeProvider; }
    // @declarations@
};
// @implementations@
int main(int argc, char** argv)
{
    assert(argc == 2);
    const int mode = std::atoi(argv[1]);
    NVSDK_NGX_Handle* handle = reinterpret_cast<NVSDK_NGX_Handle*>(0x1);
    auto create = [&]()
    {
        if (mode == 0 || mode == 3)
            return Nvngx_FG::D3D12_CreateFeature(nullptr, 0, nullptr, &handle);
        if (mode == 1 || mode == 4)
            return Nvngx_FG::VULKAN_CreateFeature(nullptr, 0, nullptr, &handle);
        return Nvngx_FG::VULKAN_CreateFeature1(nullptr, nullptr, 0, nullptr, &handle);
    };
    auto release = [&]()
    { return mode == 3 ? Nvngx_FG::D3D12_ReleaseFeature(handle) : Nvngx_FG::VULKAN_ReleaseFeature(handle); };
    if (mode == 5)
    {
        activeProvider = nullptr;
        assert(Nvngx_FG::D3D12_CreateFeature(nullptr, 0, nullptr, &handle) == NVSDK_NGX_Result_Fail);
        assert(handle == nullptr && living == 0);
        handle = reinterpret_cast<NVSDK_NGX_Handle*>(0x1);
        assert(Nvngx_FG::VULKAN_CreateFeature(nullptr, 0, nullptr, &handle) == NVSDK_NGX_Result_Fail);
        assert(handle == nullptr && living == 0);
        handle = reinterpret_cast<NVSDK_NGX_Handle*>(0x1);
        assert(Nvngx_FG::VULKAN_CreateFeature1(nullptr, nullptr, 0, nullptr, &handle) == NVSDK_NGX_Result_Fail);
        assert(handle == nullptr && living == 0);
        return 0;
    }
    if (mode == 6)
    {
        assert(Nvngx_FG::D3D12_CreateFeature(nullptr, 0, nullptr, nullptr) == NVSDK_NGX_Result_FAIL_InvalidParameter);
        assert(Nvngx_FG::VULKAN_CreateFeature(nullptr, 0, nullptr, nullptr) == NVSDK_NGX_Result_FAIL_InvalidParameter);
        assert(Nvngx_FG::VULKAN_CreateFeature1(nullptr, nullptr, 0, nullptr, nullptr) ==
               NVSDK_NGX_Result_FAIL_InvalidParameter);
        assert(living == 0);
        return 0;
    }
    if (mode < 3)
    {
        provider.createResult = NVSDK_NGX_Result_Fail;
        assert(create() == NVSDK_NGX_Result_Fail);
        assert(handle == nullptr && "failed creation must not publish a proxy handle");
        assert(living == 0 && held == 0);
    }
    else
    {
        assert(create() == NVSDK_NGX_Result_Success && handle != nullptr);
        provider.releaseResult = NVSDK_NGX_Result_Fail;
        assert(release() == NVSDK_NGX_Result_Fail);
        assert(provider.releases == 1 && living == 1 && held == 0);
        provider.releaseResult = NVSDK_NGX_Result_Success;
        assert(release() == NVSDK_NGX_Result_Success);
        assert(provider.releases == 2 && living == 0 && held == 0);
    }
    std::puts("PASS");
}
