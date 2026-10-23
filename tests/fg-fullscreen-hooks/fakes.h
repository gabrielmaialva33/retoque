// Fakes for the FG swapchain's fullscreen hooks, sliced verbatim from OptiScaler/hooks/FG_Hooks.cpp.
// The originals below follow DXGI's documented contract: both GetFullscreenState arguments are optional, the
// target is AddRef'd for the caller and is NULL when windowed, and GetFullscreenDesc refuses a NULL description.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

using HRESULT = int32_t;
using BOOL = int;
using UINT = unsigned int;
using LONG_PTR = intptr_t;
using HWND = void*;
constexpr BOOL TRUE = 1;
constexpr BOOL FALSE = 0;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = (HRESULT) 0x80004005u;
constexpr HRESULT DXGI_ERROR_INVALID_CALL = (HRESULT) 0x887A0001u;
#define SUCCEEDED(hr) (((HRESULT) (hr)) >= 0)
#define FAILED(hr) (((HRESULT) (hr)) < 0)
#define LOG_DEBUG(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)

struct IDXGIOutput
{
    int refs = 1;
    unsigned long AddRef() { return (unsigned long) ++refs; }
    unsigned long Release() { return (unsigned long) --refs; }
};

struct DXGI_MODE_DESC
{
    UINT Width = 0;
    UINT Height = 0;
};
struct DXGI_SWAP_CHAIN_DESC
{
    DXGI_MODE_DESC BufferDesc;
    UINT BufferCount = 2;
    UINT Flags = 0;
    BOOL Windowed = 1;
};
struct DXGI_SWAP_CHAIN_FULLSCREEN_DESC
{
    BOOL Windowed = 1;
};

// The real chain behind the hooks: what DXGI would answer.
struct IDXGISwapChain
{
    BOOL realFullscreen = 0;
    IDXGIOutput* realTarget = nullptr;
    IDXGIOutput* containing = nullptr; // the output that holds the window; nullptr makes GetContainingOutput fail
    UINT width = 1920;
    UINT height = 1080;
    int containingCalls = 0;

    HRESULT GetDesc(DXGI_SWAP_CHAIN_DESC* desc)
    {
        desc->BufferDesc.Width = width;
        desc->BufferDesc.Height = height;
        desc->Windowed = !realFullscreen;
        return S_OK;
    }

    HRESULT GetContainingOutput(IDXGIOutput** ppOutput)
    {
        ++containingCalls;
        if (containing == nullptr)
        {
            *ppOutput = nullptr;
            return E_FAIL;
        }
        containing->AddRef();
        *ppOutput = containing;
        return S_OK;
    }
};
struct IDXGISwapChain1 : IDXGISwapChain
{
};

struct Calls
{
    int setState = 0;
    int getState = 0;
    int getDesc = 0;
    int resizeTarget = 0;
    BOOL lastSetFullscreen = -1;
    IDXGIOutput* lastSetTarget = nullptr;
    BOOL* lastGetFullscreenArg = nullptr;
    IDXGIOutput** lastGetTargetArg = nullptr;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC* lastGetDescArg = nullptr;
    bool descNullAnswersOk = false; // a proxy that answers S_OK to a NULL description instead of refusing it
};
inline Calls calls;

inline HRESULT FakeSetFullscreenState(IDXGISwapChain* This, BOOL Fullscreen, IDXGIOutput* pTarget)
{
    ++calls.setState;
    calls.lastSetFullscreen = Fullscreen;
    calls.lastSetTarget = pTarget;
    This->realFullscreen = Fullscreen;
    This->realTarget = Fullscreen ? (pTarget != nullptr ? pTarget : This->containing) : nullptr;
    return S_OK;
}

inline HRESULT FakeGetFullscreenState(IDXGISwapChain* This, BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    ++calls.getState;
    calls.lastGetFullscreenArg = pFullscreen;
    calls.lastGetTargetArg = ppTarget;
    if (pFullscreen != nullptr)
        *pFullscreen = This->realFullscreen;
    if (ppTarget != nullptr)
    {
        *ppTarget = This->realFullscreen ? This->realTarget : nullptr;
        if (*ppTarget != nullptr)
            (*ppTarget)->AddRef();
    }
    return S_OK;
}

inline HRESULT FakeGetFullscreenDesc(IDXGISwapChain1* This, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    ++calls.getDesc;
    calls.lastGetDescArg = pDesc;
    if (pDesc == nullptr)
        return calls.descNullAnswersOk ? S_OK : DXGI_ERROR_INVALID_CALL;
    pDesc->Windowed = !This->realFullscreen;
    return S_OK;
}

inline HRESULT FakeResizeTarget(IDXGISwapChain*, const DXGI_MODE_DESC*)
{
    ++calls.resizeTarget;
    return S_OK;
}

// Window styling the hooks do on the game's window.
constexpr int GWL_STYLE = -16;
constexpr int GWL_EXSTYLE = -20;
constexpr LONG_PTR WS_POPUP = 0x80000000L;
constexpr LONG_PTR WS_VISIBLE = 0x10000000L;
constexpr LONG_PTR WS_OVERLAPPEDWINDOW = 0x00CF0000L;
constexpr LONG_PTR WS_EX_APPWINDOW = 0x00040000L;
constexpr LONG_PTR WS_EX_OVERLAPPEDWINDOW = 0x00000300L;
constexpr UINT SWP_NOZORDER = 0x0004;
constexpr UINT SWP_NOACTIVATE = 0x0010;
constexpr UINT SWP_FRAMECHANGED = 0x0020;
constexpr UINT SWP_SHOWWINDOW = 0x0040;
inline const HWND HWND_TOP = nullptr;

struct Window
{
    LONG_PTR style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    int restyles = 0;
    int moves = 0;
};
inline Window window;

inline LONG_PTR SetWindowLongPtr(HWND, int index, LONG_PTR value)
{
    ++window.restyles;
    if (index == GWL_STYLE)
        window.style = value;
    return 0;
}

inline BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT)
{
    ++window.moves;
    return 1;
}

namespace Util
{
struct MonitorInfo
{
    void* handle = nullptr;
    int x = 0;
    int y = 0;
    int width = 2560;
    int height = 1440;
    std::wstring name = L"fake";
};
inline MonitorInfo GetMonitorInfoForOutput(IDXGIOutput*) { return {}; }
inline MonitorInfo GetMonitorInfoForWindow(HWND) { return {}; }
} // namespace Util
inline std::string wstring_to_string(const std::wstring&) { return {}; }

struct IFGFeature
{
    bool active = false;
    int deactivations = 0;
    bool IsActive() { return active; }
    void UpdateTarget() {}
    void Deactivate()
    {
        ++deactivations;
        active = false;
    }
};

enum class FGOutput
{
    NoFG,
    FSRFG,
    DLSSG,
    XeFG,
    Reprojection
};

struct State
{
    FGOutput activeFgOutput = FGOutput::XeFG;
    IFGFeature* currentFG = nullptr;
    bool fgChanged = false;
    bool SCExclusiveFullscreen = false;
    bool realExclusiveFullscreen = false;
    static State& Instance()
    {
        static State s;
        return s;
    }
};

struct Option
{
    bool value = false;
    bool value_or_default() { return value; }
};
struct Config
{
    Option FGXeFGForceBorderless;
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};

using PFN_SetFullscreenState = HRESULT (*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using PFN_GetFullscreenState = HRESULT (*)(IDXGISwapChain*, BOOL*, IDXGIOutput**);
using PFN_GetFullscreenDesc = HRESULT (*)(IDXGISwapChain1*, DXGI_SWAP_CHAIN_FULLSCREEN_DESC*);
using PFN_ResizeTarget = HRESULT (*)(IDXGISwapChain*, const DXGI_MODE_DESC*);

struct FGHooks
{
    static HRESULT hkSetFullscreenState(IDXGISwapChain* This, BOOL Fullscreen, IDXGIOutput* pTarget);
    static HRESULT hkGetFullscreenState(IDXGISwapChain* This, BOOL* pFullscreen, IDXGIOutput** ppTarget);
    static HRESULT hkGetFullscreenDesc(IDXGISwapChain1* This, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc);
    static HRESULT hkResizeTarget(IDXGISwapChain* This, const DXGI_MODE_DESC* pNewTargetParameters);
    static void CoverAfterResizeBuffers(HRESULT result);
    static void CoverAfterResizeBuffers1(HRESULT result);

    inline static PFN_SetFullscreenState o_FGSCSetFullscreenState = FakeSetFullscreenState;
    inline static PFN_GetFullscreenState o_FGSCGetFullscreenState = FakeGetFullscreenState;
    inline static PFN_GetFullscreenDesc o_FGSCGetFullscreenDesc = FakeGetFullscreenDesc;
    inline static PFN_ResizeTarget o_FGSCResizeTarget = FakeResizeTarget;
    inline static HWND _hwnd = (HWND) 0x1234;
};
