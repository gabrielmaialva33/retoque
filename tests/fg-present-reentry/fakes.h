// Fakes for the wrapped swapchain's present lock, sliced verbatim from OptiScaler/wrapped/wrapped_swapchain.cpp and
// OptiScaler/OwnedMutex.h. Only DXGI and OptiScaler's surroundings are faked; the lock code is production code.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <thread>

template <typename... T> void testLog(T&&...) {}
#define LOG_TRACE(...) testLog(__VA_ARGS__)
#define LOG_DEBUG(...) testLog(__VA_ARGS__)
#define LOG_INFO(...) testLog(__VA_ARGS__)
#define LOG_WARN(...) testLog(__VA_ARGS__)
#define LOG_ERROR(...) testLog(__VA_ARGS__)
#define STDMETHODCALLTYPE
#define USE_LOCAL_MUTEX

// @owned-mutex@

using HRESULT = int32_t;
using BOOL = int;
using UINT = unsigned int;
using HWND = void*;
using IUnknown = void;
using ID3D12CommandQueue = void;
using IDXGIOutput = void;
enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0
};
struct DXGI_PRESENT_PARAMETERS
{
};
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = (HRESULT) 0x80004005u;
constexpr HRESULT DXGI_ERROR_DEVICE_REMOVED = (HRESULT) 0x887A0005u;
constexpr UINT DXGI_PRESENT_TEST = 0x1;
inline void* _ReturnAddress() { return nullptr; }

namespace Util
{
inline std::string WhoIsTheCaller(void*) { return "test"; }
} // namespace Util

enum class FGOutput
{
    NoFG,
    FSRFG,
    DLSSG,
    XeFG
};
enum class FGNvngxReplacement
{
    None,
    DLSSGMod
};

struct Setting
{
    bool value = true;
    bool value_or_default() const { return value; }
};
struct Config
{
    Setting FGUseMutexForSwapchain;
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};

// OptiScaler's own FG object: only its swapchain mutex and the active flag are read here
struct FG
{
    OwnedMutex Mutex;
    bool active = true;
    bool IsActive() const { return active; }
};

struct State
{
    FG* currentFG = nullptr;
    FGOutput activeFgOutput = FGOutput::NoFG;
    FGNvngxReplacement activeFgNvngx = FGNvngxReplacement::None;
    ID3D12CommandQueue* currentCommandQueue = nullptr;
    bool realExclusiveFullscreen = false;
    bool reflexLimitsFps = true; // no frame limiter sleep inside the tests
    static State& Instance()
    {
        static State s;
        return s;
    }
};

namespace IdentifyGpu
{
struct Gpu
{
    bool dxgiIsDxvk = false;
};
inline Gpu getPrimaryGpu() { return {}; }
} // namespace IdentifyGpu
namespace InputCommon
{
inline bool can_limit_fps() { return true; }
} // namespace InputCommon
namespace FrameLimit
{
inline void sleep(bool) {}
} // namespace FrameLimit

// What runs inside the real present (DXGI delivering a window message, a provider's present) and the device work of
// the methods that wait for present
static std::function<void()> insidePresent = [] {};
static std::function<HRESULT()> fullscreenWork = [] { return S_OK; };
static std::function<HRESULT()> resizeWork = [] { return S_OK; };

struct FakeSwapchain
{
    HRESULT Present(UINT, UINT) { return S_OK; }
    HRESULT Present1(UINT, UINT, const DXGI_PRESENT_PARAMETERS*) { return S_OK; }
    HRESULT SetFullscreenState(BOOL, IDXGIOutput*) { return fullscreenWork(); }
};
using IDXGISwapChain = FakeSwapchain;
using IDXGISwapChain1 = FakeSwapchain;

static HRESULT LocalPresent(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*, IUnknown*, HWND, bool)
{
    insidePresent();
    return S_OK;
}

class WrappedIDXGISwapChain4
{
  public:
    HRESULT Present(UINT SyncInterval, UINT Flags);
    HRESULT Present1(UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
    HRESULT SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget);
    HRESULT ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
    HRESULT ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags,
                           const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue);

    FakeSwapchain real;
    IDXGISwapChain* _real = &real;
    IDXGISwapChain1* _real1 = &real;
    IUnknown* _device = nullptr;
    HWND _handle = nullptr;
    bool _uwp = false;
    OwnedMutex _localMutex;
};
