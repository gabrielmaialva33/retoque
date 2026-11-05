#include "pch.h"

// Compile the complete production scene entry against inert graphics interfaces. The picture rule,
// geometry binding and scene control flow are real; no GPU ordering or model behavior is simulated.
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define FAILED(x) ((x) != 0)
#define IID_PPV_ARGS(x) (x)
constexpr int NVSDK_NGX_Result_Success = 0;
#define PARAM(name) constexpr const char* NVSDK_NGX_Parameter_##name = #name
PARAM(Color);
PARAM(Output);
PARAM(Depth);
PARAM(MotionVectors);
PARAM(ExposureTexture);
PARAM(Width);
PARAM(Height);
PARAM(OutWidth);
PARAM(OutHeight);
PARAM(DLSS_Render_Subrect_Dimensions_Width);
PARAM(DLSS_Render_Subrect_Dimensions_Height);
PARAM(DLSS_Output_Subrect_Base_X);
PARAM(DLSS_Output_Subrect_Base_Y);
PARAM(DLSS_Pre_Exposure);
#undef PARAM
struct NVSDK_NGX_Parameter
{
    std::unordered_map<std::string, uintptr_t> values;
    mutable unsigned reads = 0;
    template <class T> int Get(const char* key, T* out) const
    {
        ++reads;
        const auto it = values.find(key);
        if (it == values.end())
            return 1;
        if constexpr (std::is_pointer_v<T>)
            *out = reinterpret_cast<T>(it->second);
        else
            *out = static_cast<T>(it->second);
        return 0;
    }
};
#include <dlssnr/DlssNr_RenderGeometry.h>
#include <dlssnr/DlssNr_PictureGate.h>
#include <dlssnr/DlssNr_PresentBoundary.h>
#include <dlssnr/DlssNr_Dx11ScenePolicy.h>

namespace Microsoft::WRL
{
template <class T> struct ComPtr
{
    T* value = nullptr;
    T* Get() const { return value; }
    T* operator->() const { return value; }
    T** operator&() { return &value; }
    explicit operator bool() const { return value != nullptr; }
    void Reset() { value = nullptr; }
    ComPtr& operator=(T* ptr)
    {
        value = ptr;
        return *this;
    }
};
} // namespace Microsoft::WRL
struct ID3D11Texture2D;
struct ID3D11Resource
{
    int QueryInterface(ID3D11Texture2D** out);
};
struct D3D11_TEXTURE2D_DESC
{
    unsigned Width = 1920, Height = 1080;
};
struct ID3D11Texture2D : ID3D11Resource
{
    void GetDesc(D3D11_TEXTURE2D_DESC* out) { *out = {}; }
};
int ID3D11Resource::QueryInterface(ID3D11Texture2D** out)
{
    *out = static_cast<ID3D11Texture2D*>(this);
    return 0;
}
struct ID3D11Device
{
};
struct ID3D11DeviceContext
{
    ID3D11Device device;
    unsigned reads = 0;
    void GetDevice(ID3D11Device** out)
    {
        ++reads;
        *out = &device;
    }
};
struct ID3D12Resource
{
};
struct ID3D12GraphicsCommandList
{
};
struct ID3D12CommandQueue
{
};
struct D3D12_COMMAND_QUEUE_DESC
{
    int Type = 0;
};
constexpr int D3D12_COMMAND_LIST_TYPE_DIRECT = 1;
constexpr int D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 2;
constexpr int D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 3;
struct ID3D12Device
{
    ID3D12CommandQueue queue;
    int CreateCommandQueue(D3D12_COMMAND_QUEUE_DESC*, ID3D12CommandQueue** out)
    {
        *out = &queue;
        return 0;
    }
};
namespace WithDx12
{
inline ID3D12Device device;
inline ID3D12CommandQueue present;
inline unsigned prepares = 0;
bool PrepareD3D12ForD3D11(ID3D11Device*)
{
    ++prepares;
    return true;
}
ID3D12Device* GetD3D12Device() { return &device; }
ID3D12CommandQueue* GetD3D12CommandQueue() { return &present; }
} // namespace WithDx12
enum class Upscaler
{
    DLSS,
    Other
};
template <class T> struct Setting
{
    T value;
    bool has_value() const { return true; }
    T value_or_default() const { return value; }
};
struct Config
{
    bool startup = true;
    Setting<bool> DlssNrEnabled { true };
    Setting<Upscaler> Dx11Upscaler { Upscaler::DLSS };
    Setting<unsigned> DlssNrHookMethod { 1 };
    Setting<int> DlssNrStage { 0 };
    bool DlssNrDx11SceneStageAtStart() const { return startup; }
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};
namespace sl
{
enum class DLSSGMode
{
    eOff,
    eOn
};
}
enum class FGInput
{
    NoFG,
    NvngxFG,
    ForceXeLL
};
enum class FGOutput
{
    NoFG,
    FSRFG
};
struct FakeFG
{
    bool active = true, paused = false;
    bool IsActive() const { return active; }
    bool IsPaused() const { return paused; }
};
struct State
{
    FakeFG* currentFG = nullptr;
    FakeFG* currentVkFG = nullptr;
    sl::DLSSGMode dlssgLastSetMode = sl::DLSSGMode::eOff;
    int dlssgDetectedInterpolationCount = 0;
    std::atomic<bool> dlssgGameRequestsFg { false };
    bool fsrfgInputActive = false, dlssgInputGameEnabled = false;
    FGInput activeFgInput = FGInput::NoFG;
    FGOutput activeFgOutput = FGOutput::NoFG;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
namespace DlssNr
{
struct DlssNrFrameInfo
{
    ID3D12Resource* ExposureTexture = nullptr;
    float PreExposure = 1;
    bool NativeDx11Scene = false, Reset = false;
    int OutputState = 0, DepthState = 0, MotionState = 0;
    unsigned DisplayWidth = 1920, DisplayHeight = 1080;
    uint64_t PictureEpoch = 0;
};
namespace Submission
{
struct Usage
{
};
bool Track(ID3D12GraphicsCommandList*, Usage&) { return true; }
} // namespace Submission
inline std::vector<DlssNrFrameInfo> frames;
DlssNrFrameInfo GatherGuideFrame(NVSDK_NGX_Parameter* params)
{
    DlssNrFrameInfo frame;
    frame.PictureEpoch = Pictures::EvaluateEpoch(params);
    return frame;
}
bool EvaluateDx11SceneFrame(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                            const DlssNrFrameInfo& frame, ID3D12CommandQueue*, const char**)
{
    frames.push_back(frame);
    return true;
}
namespace Dx11Scene
{
struct Transport
{
    struct Evidence
    {
        uintptr_t nr = 1, consumer = 2, present = 3;
        unsigned nrFamily = 0, consumerFamily = 0, presentFamily = 0;
    } evidence;
    enum class Result
    {
        Recorded,
        Busy,
        Passthrough,
        Failed
    };
    inline static unsigned runs = 0;
    inline static ID3D11Texture2D* target = nullptr;
    inline static std::function<void()> duringRun;
    inline static Result next = Result::Recorded;
    bool Init(ID3D11Device*, ID3D11DeviceContext*, ID3D12Device*, ID3D12CommandQueue*, ID3D12CommandQueue*)
    {
        return true;
    }
    const char* Reason() const { return "test transport"; }
    bool Retire() { return true; }
    template <class Fn> Result Run(const std::array<ID3D11Texture2D*, 4>& inputs, Fn record, void* data)
    {
        ++runs;
        target = inputs[0];
        if (duringRun)
            duringRun();
        if (next != Result::Recorded)
            return next;
        std::array<ID3D12Resource*, 4> resources {};
        record(nullptr, nullptr, resources, data);
        return Result::Recorded;
    }
};
} // namespace Dx11Scene
} // namespace DlssNr
#include "scene-under-test.inc"

using namespace DlssNr;
using T = Dx11Scene::Transport;
static ID3D11Texture2D colour1, colour2, output1, output2, depth, motion;
static ID3D11DeviceContext context;
constexpr auto key1 = RenderGeometry::Key(RenderGeometry::Api::D3D11, 1);
constexpr auto key2 = RenderGeometry::Key(RenderGeometry::Api::D3D11, 2);
static NVSDK_NGX_Parameter Params(ID3D11Texture2D* colour, ID3D11Texture2D* output)
{
    return { { { "Color", reinterpret_cast<uintptr_t>(colour) },
               { "Output", reinterpret_cast<uintptr_t>(output) },
               { "Depth", reinterpret_cast<uintptr_t>(&depth) },
               { "MotionVectors", reinterpret_cast<uintptr_t>(&motion) },
               { "Width", 1280 },
               { "Height", 720 },
               { "OutWidth", 1920 },
               { "OutHeight", 1080 } } };
}
static void Remember(uint64_t key) { RenderGeometry::Remember(key, { { 1280, 720 }, { 1920, 1080 } }); }
static void Evaluate(uint64_t key, NVSDK_NGX_Parameter& params)
{
    RenderGeometry::Scope scope(Config::Instance()->DlssNrEnabled.value, key, &params);
    EvaluateNativeDx11Scene(&context, &params);
}
static void AssertReason(const char* reason)
{
    assert(NativeDx11SceneStatus() && std::string(NativeDx11SceneStatus()) == reason);
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string test = argv[1];
    auto main = Params(&colour1, &output1);
    auto other = Params(&colour2, &output1);
    if (test == "unbound")
    {
        // Feature created with NR disabled: creation is deliberately not remembered by the NGX input.
        Config::Instance()->DlssNrEnabled.value = false;
        Evaluate(key1, main);
        assert(main.reads == 0 && T::runs == 0);
        Config::Instance()->DlssNrEnabled.value = true;
        Evaluate(key1, main);
        Evaluate(key2, other);
        assert(T::runs == 0 && WithDx12::prepares == 0 && main.reads == 0);
        assert(Pictures::Instance().Main() == 0);
        AssertReason("Dx11SceneStage: FeatureIdentityUnavailableRecreate");
        bool readColour = false;
        const auto legacy = Pictures::Ask(&main,
                                          [&]
                                          {
                                              readColour = true;
                                              return uintptr_t(0);
                                          });
        assert(legacy.role == Pictures::Role::Main && legacy.epoch == 0 && !readColour);
        Remember(key1);
        Evaluate(key1, main);
        assert(T::runs == 1 && frames.back().PictureEpoch != 0);
        // Matching params and a nonzero key are both required, even if a binding exists.
        RenderGeometry::Binding bad { &other, {}, {}, nullptr, key1 };
        RenderGeometry::active = &bad;
        EvaluateNativeDx11Scene(&context, &main);
        assert(T::runs == 1 && !bad.judged);
        bad.params = &main;
        bad.key = 0;
        EvaluateNativeDx11Scene(&context, &main);
        assert(T::runs == 1 && !bad.judged);
        RenderGeometry::active = nullptr;
        AssertReason("Dx11SceneStage: FeatureIdentityUnavailableRecreate");
        Evaluate(key1, main);
        assert(T::runs == 2 && frames.back().Reset);
        return 0;
    }
    Remember(key1);
    Remember(key2);
    if (test == "cold")
    {
        Config::Instance()->startup = false;
        Evaluate(key1, main);
        // Scope reads dimensions, but the scene must not read resources, judge or touch status/transport.
        main.reads = 0;
        EvaluateNativeDx11Scene(&context, &main);
        assert(main.reads == 0 && context.reads == 0 && T::runs == 0 && !NativeDx11SceneStatus());
        assert(Pictures::Instance().Main() == 0);
        return 0;
    }
    if (test.rfind("fg-", 0) == 0 && test != "fg-gap")
    {
        FakeFG fg;
        auto& state = State::Instance();
        if (test == "fg-dx12")
            state.currentFG = &fg;
        else if (test == "fg-vk")
            state.currentVkFG = &fg;
        else if (test == "fg-mode")
            state.dlssgLastSetMode = sl::DLSSGMode::eOn;
        else if (test == "fg-count")
            state.dlssgDetectedInterpolationCount = 1;
        else if (test == "fg-request")
            state.dlssgGameRequestsFg = true;
        else if (test == "fg-fsr")
            state.fsrfgInputActive = true;
        else if (test == "fg-input-game")
            state.dlssgInputGameEnabled = true;
        else if (test == "fg-input")
            state.activeFgInput = FGInput::NvngxFG;
        else if (test == "fg-output")
            state.activeFgOutput = FGOutput::FSRFG;
        else
            assert(false);
        Evaluate(key1, main);
        assert(T::runs == 0 && WithDx12::prepares == 0);
        AssertReason("Dx11SceneStage: FrameGenerationUnsupported");
        return 0;
    }
    Evaluate(key1, main);
    assert(T::runs == 1 && frames.back().Reset && T::target == &output1);
    const auto epoch = frames.back().PictureEpoch;
    if (test == "different-colours")
    {
        Evaluate(key2, other);
        assert(T::runs == 1);
    }
    else if (test == "same-colour")
    {
        other = Params(&colour1, &output2);
        Evaluate(key2, other);
        assert(T::runs == 2 && T::target == &output2 && frames.back().PictureEpoch == epoch);
    }
    else if (test == "disabled-gap")
    {
        Config::Instance()->DlssNrEnabled.value = false;
        for (int i = 0; i < 4; ++i)
            Evaluate(key1, main);
        assert(T::runs == 1);
        Config::Instance()->DlssNrEnabled.value = true;
        Evaluate(key2, other); // An Aside must neither run nor consume the pending main reset.
        assert(T::runs == 1);
        Evaluate(key1, main);
        assert(T::runs == 2 && frames.back().Reset && frames.back().PictureEpoch == epoch);
    }
    else if (test == "aside")
    {
        Evaluate(key2, other);
        Config::Instance()->DlssNrStage.value = 1;
        Evaluate(key2, other); // Known Aside on an ineligible stage is not a gap in the main picture.
        Config::Instance()->DlssNrStage.value = 0;
        assert(T::runs == 1);
    }
    else if (test == "stage-gap")
    {
        Config::Instance()->DlssNrStage.value = 1;
        Evaluate(key1, main);
        Config::Instance()->DlssNrStage.value = 0;
        Evaluate(key1, main);
        assert(T::runs == 2 && frames.back().Reset && frames.back().PictureEpoch == epoch);
    }
    else if (test == "fg-gap")
    {
        State::Instance().dlssgDetectedInterpolationCount = 1;
        Evaluate(key1, main);
        assert(T::runs == 1);
        AssertReason("Dx11SceneStage: FrameGenerationUnsupported");
        State::Instance().dlssgDetectedInterpolationCount = 0;
        Evaluate(key1, main);
        assert(T::runs == 2 && frames.back().Reset && frames.back().PictureEpoch == epoch);
    }
    else if (test == "busy")
    {
        T::next = T::Result::Busy;
        Evaluate(key1, main);
        T::next = T::Result::Recorded;
        Evaluate(key1, main);
        assert(frames.back().Reset && frames.back().PictureEpoch == epoch);
    }
    else if (test == "concurrent-gap")
    {
        T::duringRun = [&]
        {
            // A same-picture evaluate contends while the earlier one is recording.
            std::thread contender([&] { Evaluate(key1, main); });
            contender.join();
        };
        Evaluate(key1, main);
        T::duringRun = nullptr;
        assert(T::runs == 2);
        Evaluate(key1, main);
        assert(T::runs == 3 && frames.back().Reset && frames.back().PictureEpoch == epoch);
    }
    else
        assert(false);
    Evaluate(key1, main);
    assert(!frames.back().Reset && frames.back().PictureEpoch == epoch);
    std::puts("PASS: production scene control flow");
}
