// Fakes for the D3D12 upscalers' shared legacy menu (OverlayMenu=false), sliced verbatim from
// OptiScaler/upscalers/IFeature_Dx12.{h,cpp}: the static menu and its lock declarations, Init, RenderLegacyMenu and
// the destructor. The menu and the shader passes are fakes that only count and check that they are alive.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using HWND = void*;
struct ID3D12Device
{
};
struct ID3D12GraphicsCommandList
{
};
struct ID3D12Resource
{
};
struct NVSDK_NGX_Parameter
{
};

enum class Upscaler
{
    XeSS,
    FSR21,
    DLSS,
    DLSSD
};

struct Setting
{
    bool value = false;
    bool value_or_default() const { return value; }
};
struct Config
{
    Setting OverlayMenu; // false: the legacy in-upscaler menu this suite is about
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};
struct State
{
    bool isShuttingDown = false;
    static State& Instance()
    {
        static State s;
        return s;
    }
};
namespace Util
{
inline HWND GetProcessWindow() { return reinterpret_cast<HWND>(0x10); }
} // namespace Util

// The menu: Render runs the test's hook while it is drawing; a destroyed menu is poisoned and freed, so drawing with
// one ASan reports as a heap use after free
static std::atomic<int> menusAlive { 0 };
static std::atomic<int> menusCreated { 0 };
static std::function<void()> duringRender = [] {};
static std::function<void()> duringDestroy = [] {};
constexpr unsigned kAlive = 0xA11CEu;
struct Menu_Dx12
{
    unsigned magic = kAlive;
    Menu_Dx12(HWND, ID3D12Device*)
    {
        ++menusAlive;
        ++menusCreated;
    }
    ~Menu_Dx12()
    {
        duringDestroy();
        magic = 0;
        --menusAlive;
    }
    bool IsHandleDifferent() const { return false; }
    void Render(ID3D12GraphicsCommandList*, ID3D12Resource*)
    {
        duringRender();
        if (magic != kAlive)
        {
            std::fprintf(stderr, "FAIL: the menu was destroyed while it was being drawn\n");
            std::_Exit(1);
        }
    }
};

struct Pass
{
    Pass(const char*, ID3D12Device*, bool = false) {}
};
using OS_Dx12 = Pass;
using RCAS_Dx12 = Pass;
using Bias_Dx12 = Pass;
using Magnifier_Dx12 = Pass;
struct GpuTime_Dx12
{
    explicit GpuTime_Dx12(ID3D12Device*) {}
};

class IFeature_Dx12
{
  protected:
    ID3D12Device* Device = nullptr;
    // @menu-declarations@
    std::unique_ptr<OS_Dx12> OutputScaler = nullptr;
    std::unique_ptr<RCAS_Dx12> RCAS = nullptr;
    std::unique_ptr<Bias_Dx12> Bias = nullptr;
    std::unique_ptr<Magnifier_Dx12> Magnifier = nullptr;
    std::unique_ptr<GpuTime_Dx12> UpscalerTime = nullptr;

    bool InitInternal(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*) { return true; }
    Upscaler GetUpscalerType() const { return Upscaler::XeSS; }
    unsigned TargetWidth() const { return 1280; }
    unsigned DisplayWidth() const { return 1920; }

  public:
    bool Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);
    void RenderLegacyMenu(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InOutput);
    bool HasMenu() const { return Imgui != nullptr; }
    ~IFeature_Dx12();
};
