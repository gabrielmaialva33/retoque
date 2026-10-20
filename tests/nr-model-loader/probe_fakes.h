#pragma once
// Probe control-flow fixture. No Windows loader, COM object or model is executed.
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <cstdio>

#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define IID_PPV_ARGS(p) (p)
#define SUCCEEDED(hr) ((hr) >= 0)
#define FAILED(hr) ((hr) < 0)

struct OptionalBool
{
    bool value = false;
    bool value_or_default() const { return value; }
};
struct Config
{
    OptionalBool DlssNrProbeD3D11;
    static Config* Instance()
    {
        static Config cfg;
        return &cfg;
    }
};
struct State
{
    std::wstring NVNGX_ApplicationDataPath = L"fixture";
    static State& Instance()
    {
        static State state;
        return state;
    }
};

struct Counters
{
    int ensures = 0, symbols = 0, probes = 0, requirements = 0, inits = 0;
    int queried = 0, dxgiReleased = 0, gameReleased = 0, fallbackReleased = 0, factoryReleased = 0;
    bool direct = false, ensureFails = false, missingExports = false, missingSnippet = false;
    bool queryFails = false, adapterFails = false, incomplete = false;
};
inline Counters calls;
struct DXGI_ADAPTER_DESC
{
    unsigned VendorId = 0, DeviceId = 0;
};
struct IDXGIAdapter
{
    bool fallback;
    int GetDesc(DXGI_ADAPTER_DESC* d)
    {
        d->VendorId = 0x10de;
        d->DeviceId = fallback ? 2 : 1;
        return 0;
    }
    void Release() { fallback ? ++calls.fallbackReleased : ++calls.gameReleased; }
};
inline IDXGIAdapter gameAdapter { false }, fallbackAdapter { true };
struct IDXGIDevice
{
    int GetAdapter(IDXGIAdapter** out)
    {
        *out = calls.adapterFails ? nullptr : &gameAdapter;
        return calls.adapterFails ? -1 : 0;
    }
    void Release() { ++calls.dxgiReleased; }
};
inline IDXGIDevice dxgi;
struct IUnknown
{
    int QueryInterface(IDXGIDevice** out)
    {
        ++calls.queried;
        *out = calls.queryFails ? nullptr : &dxgi;
        return calls.queryFails ? -1 : 0;
    }
};
struct IDXGIFactory1
{
    int EnumAdapters(unsigned index, IDXGIAdapter** out)
    {
        assert(index == 0);
        *out = &fallbackAdapter;
        return 0;
    }
    void Release() { ++calls.factoryReleased; }
};
inline IDXGIFactory1 factory;
inline int CreateDXGIFactory1(IDXGIFactory1** out)
{
    *out = &factory;
    return 0;
}
namespace Util
{
inline std::filesystem::path ExePath() { return "fixture/game.exe"; }
inline std::optional<std::filesystem::path> FindFilePath(const std::filesystem::path&, const char*)
{
    if (calls.missingSnippet)
        return std::nullopt;
    return "fixture/model.dll";
}
} // namespace Util
namespace DlssNr::Dx12Internal
{
struct NrState
{
    void* forwarder = nullptr;
};
extern NrState g_nr;
extern std::mutex g_nrMutex;
extern std::filesystem::path g_dllDir;
bool EnsureForwarder();
const char* NgxResultName(unsigned int);
} // namespace DlssNr::Dx12Internal
void* GetProcAddress(void* module, const char* name);
namespace DlssNr
{
void ProbeD3D11(void* device);
}
