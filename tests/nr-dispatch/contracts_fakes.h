#include "retoque/shaders/dlssnr/DlssNr_GuideMatch.h"
#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

int failures = 0, allocations = 0, failAt = 0, evaluations = 0, dispatches = 0, destroys = 0, barriers = 0;
bool shaderAvailable = true, initOk = true, dispatchOk = true;
void check(bool ok, const char* why)
{
    if (!ok)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", why);
    }
}
struct Desc
{
    int Format;
    unsigned Width, Height;
};
struct ID3D12Resource
{
    Desc desc {};
};
ID3D12Resource gameDepth, gameMotion;
std::vector<std::unique_ptr<ID3D12Resource>> resources;
struct Device
{
    int releases = 0;
    void Release() { ++releases; }
} fakeDevice;
Device* device = &fakeDevice;
void* cmdList = nullptr;
constexpr int DXGI_FORMAT_R32_FLOAT = 1, DXGI_FORMAT_R32G32_FLOAT = 2;
constexpr int D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 2;
ID3D12Resource* CreateScratch(Device*, int format, unsigned w, unsigned h)
{
    if (++allocations == failAt)
        return nullptr;
    resources.push_back(std::make_unique<ID3D12Resource>(Desc { format, w, h }));
    return resources.back().get();
}
std::vector<ID3D12Resource*> parked;
void ParkNrResource(ID3D12Resource*& r)
{
    if (r)
        parked.push_back(r);
    r = nullptr;
}
void Barrier(void*, ID3D12Resource*, int, int) { ++barriers; }
struct DlssNr_GuideMatch_Dx12
{
    bool init;
    static bool Available() { return shaderAvailable; }
    DlssNr_GuideMatch_Dx12(const char*, Device*) : init(initOk) {}
    ~DlssNr_GuideMatch_Dx12() { ++destroys; }
    bool IsInit() const { return init; }
    template <class... T> bool Dispatch(T...)
    {
        ++dispatches;
        return dispatchOk;
    }
};
struct Nr
{
    ID3D12Resource *output = nullptr, *colorCopy = nullptr, *hdrCopy = nullptr, *colorSmall = nullptr;
    ID3D12Resource *depthMatched = nullptr, *motionMatched = nullptr;
    DlssNr_GuideMatch_Dx12* guideMatch = nullptr;
    unsigned workWidth = 0, workHeight = 0;
    bool failed = false, reset = false;
    std::array<bool, 4> passReset {};
    const char* reason = nullptr;
} g_nr;
struct Option
{
    bool value = true;
    bool value_or_default() const { return value; }
};
struct Config
{
    Option DlssNrMatchGuides;
} cfg;
std::string reason;
void reportSkip(const char* why) { reason = why; }
#define LOG_WARN(...) ((void) 0)
void fresh()
{
    delete g_nr.guideMatch;
    g_nr = {};
    resources.clear();
    parked.clear();
    allocations = failAt = evaluations = dispatches = destroys = barriers = 0;
    shaderAvailable = initOk = dispatchOk = true;
    fakeDevice.releases = 0;
    reason.clear();
}
