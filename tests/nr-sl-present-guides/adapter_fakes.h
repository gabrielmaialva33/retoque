#include <sl.h>
#include <dlssnr/DlssNr_SlPresentPolicy.h>
#include <dlssnr/DlssNr_PresentGuideIdentity.h>
#include <shaders/dlssnr/DlssNr_Common.h>
#include <mutex>
#include <vector>
#include <dlssnr/DlssNr_Chain.h>
#include <cassert>
#include <cstdio>
#include <climits>
using UINT = unsigned int;
using HRESULT = long;
#define FAILED(hr) ((hr) < 0)
#define IID_PPV_ARGS(pp) 0, reinterpret_cast<void**>(pp)
constexpr unsigned D3D12_RESOURCE_STATE_COMMON = 0, D3D12_RESOURCE_STATE_COPY_SOURCE = 1,
                   D3D12_RESOURCE_STATE_DEPTH_WRITE = 2, D3D12_RESOURCE_STATE_DEPTH_READ = 4,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE = 8, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 16;
using D3D12_RESOURCE_STATES = unsigned;
enum
{
    DXGI_FORMAT_R32_FLOAT,
    DXGI_FORMAT_R32_TYPELESS,
    DXGI_FORMAT_D32_FLOAT,
    DXGI_FORMAT_R16G16_FLOAT,
    DXGI_FORMAT_R32G32_FLOAT,
    DXGI_FORMAT_UNKNOWN
};
struct D3D12_RESOURCE_DESC
{
    uint64_t Width = 1280;
    unsigned Height = 720, MipLevels = 1;
    int Format = DXGI_FORMAT_R32_FLOAT;
};
struct ID3D12Resource
{
    D3D12_RESOURCE_DESC desc;
    auto GetDesc() const { return desc; }
};
struct ID3D12GraphicsCommandList
{
    bool safe = true;
};
struct ID3D12Device
{
    int Release() { return 0; }
};
struct ID3D12CommandQueue
{
    ID3D12Device* device = nullptr;
    HRESULT GetDevice(int, void** out)
    {
        *out = device;
        return device ? 0 : -1;
    }
};
struct DXGI_SWAP_CHAIN_DESC1
{
    unsigned Width = 1920, Height = 1080;
};
struct IDXGISwapChain3
{
    DXGI_SWAP_CHAIN_DESC1 desc;
    HRESULT GetDesc1(DXGI_SWAP_CHAIN_DESC1* out)
    {
        *out = desc;
        return 0;
    }
};
template <class T> struct Option
{
    T value;
    T value_or_default() const { return value; }
};
struct Config
{
    Option<bool> DlssNrEnabled { true }, DlssNrPresentSlGuides { false };
    Option<unsigned> DlssNrHookMethod { 2 };
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};
namespace DlssNr
{
namespace Dx12Internal
{
std::mutex g_nrMutex;
struct GuideAsk
{
    PresentGuideIdentity::Decision decision;
    PresentGuideIdentity::Present present;
    bool asked = false;
};
struct Identity
{
    PresentGuideIdentity::Tracker tracker;
    PresentGuideIdentity::Consumer present { "test" };
} g_guideIdentity;
PresentGuides::Ring g_presentGuideRing;
struct Retired
{
    bool stuck = false;
};
std::vector<Retired> g_nrRetired;
long long NowMs() { return 100; }
const void* PresentGuidePresenter(IDXGISwapChain3* chain) { return chain; }
} // namespace Dx12Internal
namespace SlPresent
{
bool CopyRecording(ID3D12GraphicsCommandList* list) { return list && list->safe; }
} // namespace SlPresent
} // namespace DlssNr
using namespace DlssNr;
using namespace DlssNr::Dx12Internal;
