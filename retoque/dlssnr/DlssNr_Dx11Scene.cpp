#include "pch.h"

#include "DlssNr_Dx11Scene.h"
#include "DlssNr_Dx11SceneTransport.h"
#include "DlssNrFeature_Dx12.h"
#include "DlssNr_PictureGate.h"
#include "DlssNr_PresentBoundary.h"
#include "DlssNr_Submission.h"
#include <Config.h>
#include <State.h>
#include <with_dx12/with_dx12.h>
#include <atomic>
#include <cmath>
#include <mutex>

namespace DlssNr
{
namespace
{
using Microsoft::WRL::ComPtr;
// Lock order: scene -> renderer g_nrMutex -> driver gate. Shutdown visits scene before renderer locks.
// try_lock at the evaluate seam never waits for another evaluate or teardown. No renderer entry calls scene.
std::mutex sceneMutex;
Dx11Scene::Transport* transport = nullptr;
ComPtr<ID3D11Device> owner;
ComPtr<ID3D11DeviceContext> ownerContext;
ID3D12CommandQueue* presentIdentity = nullptr;
std::atomic<const char*> status { nullptr };
bool refused = false;
// Early declines cannot take the scene lock. Consume reset debt before recording so a concurrent
// declined main evaluate cannot have its debt erased by this submission's success.
std::atomic_bool activated { false };
std::atomic_bool missed { true };
struct SceneFrame
{
    DlssNrFrameInfo frame;
    bool tracked = false;
};

bool Eligible()
{
    const auto& cfg = *Config::Instance();
    PresentBoundary::Inputs boundary;
    boundary.hookMethod = cfg.DlssNrHookMethod.value_or_default();
    return Dx11Scene::Eligible(cfg.DlssNrDx11SceneStageAtStart(), cfg.DlssNrEnabled.value_or_default(),
                               cfg.Dx11Upscaler.has_value() && cfg.Dx11Upscaler.value_or_default() == Upscaler::DLSS,
                               PresentBoundary::EffectiveMethod(boundary), cfg.DlssNrStage.value_or_default());
}
void NoteGap()
{
    if (activated.load())
        missed = true;
}
bool FrameGenerationActive()
{
    const auto& state = State::Instance();
    // None of these extra FG queues participates in the three-queue alias proof. Treat selected
    // FG routes and paused-but-active providers conservatively too; their queues may still exist.
    return (state.currentFG && state.currentFG->IsActive()) || (state.currentVkFG && state.currentVkFG->IsActive()) ||
           state.dlssgLastSetMode != sl::DLSSGMode::eOff || state.dlssgDetectedInterpolationCount > 0 ||
           state.dlssgGameRequestsFg.load() || state.fsrfgInputActive || state.dlssgInputGameEnabled ||
           state.activeFgOutput != FGOutput::NoFG ||
           (state.activeFgInput != FGInput::NoFG && state.activeFgInput != FGInput::ForceXeLL);
}
void Refuse(const char* why)
{
    refused = true;
    status = why;
    LOG_WARN("DLSS-NR {}. Scene stage disabled for this session; existing D3D11 routing retained", why);
}
ID3D11Resource* Resource(NVSDK_NGX_Parameter* params, const char* name)
{
    ID3D11Resource* value = nullptr;
    if (params->Get(name, &value) != NVSDK_NGX_Result_Success)
        params->Get(name, reinterpret_cast<void**>(&value));
    return value;
}
bool Record(ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue, const std::array<ID3D12Resource*, 4>& resources,
            void* data)
{
    auto& call = *static_cast<SceneFrame*>(data);
    Submission::Usage probe;
    call.tracked = Submission::Track(list, probe);
    if (!call.tracked)
        return false;
    auto& frame = call.frame;
    frame.ExposureTexture = resources[3];
    const char* reason = nullptr;
    return EvaluateDx11SceneFrame(list, resources[0], resources[1], resources[2], frame, queue, &reason);
}
} // namespace

void EvaluateNativeDx11Scene(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* params)
{
    if (!Config::Instance()->DlssNrDx11SceneStageAtStart())
        return; // Cold key-off path: no picture reads, status, ownership or transport work.
    const bool eligible = Eligible();
    if (!eligible && !activated.load())
        return;
    if (!context || !params)
    {
        NoteGap();
        return;
    }
    // Unlike the older seams, this return-to-D3D11 route must prove which feature owns the frame.
    // Creation while NR was off has no binding; only recreation can supply it. Do not latch the session.
    const auto* binding = RenderGeometry::active;
    if (!binding || binding->params != params || binding->key == 0)
    {
        NoteGap();
        if (eligible)
            status = "Dx11SceneStage: FeatureIdentityUnavailableRecreate";
        return;
    }
    // Output may be redirected by downstream effects. Only the input Color identifies the picture.
    // Judge before gap bookkeeping: a legitimate Aside is not a missed frame of the main picture.
    if (Pictures::Ask(params, [&] { return reinterpret_cast<uintptr_t>(Resource(params, NVSDK_NGX_Parameter_Color)); })
            .role == Pictures::Role::Aside)
        return;
    if (!eligible)
    {
        NoteGap();
        return;
    }
    if (FrameGenerationActive())
    {
        NoteGap();
        status = "Dx11SceneStage: FrameGenerationUnsupported";
        return;
    }
    std::unique_lock lock(sceneMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        NoteGap();
        return;
    }
    if (refused)
        return;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (transport && (device.Get() != owner.Get() || context != ownerContext.Get() ||
                      presentIdentity != WithDx12::GetD3D12CommandQueue()))
    {
        Refuse("Dx11SceneStage: DeviceOrPresentQueueChanged");
        return;
    }
    if (!transport)
    {
        if (!WithDx12::PrepareD3D12ForD3D11(device.Get()))
        {
            Refuse("Dx11SceneStage: D3D12Unavailable");
            return;
        }
        auto d12 = WithDx12::GetD3D12Device();
        auto present = WithDx12::GetD3D12CommandQueue();
        ComPtr<ID3D12CommandQueue> nr;
        D3D12_COMMAND_QUEUE_DESC desc {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!d12 || !present || FAILED(d12->CreateCommandQueue(&desc, IID_PPV_ARGS(&nr))))
        {
            Refuse("Dx11SceneStage: NrQueueUnavailable");
            return;
        }
        auto candidate = std::make_unique<Dx11Scene::Transport>();
        if (!candidate->Init(device.Get(), context, d12, nr.Get(), present))
        {
            Refuse(candidate->Reason());
            return;
        }
        owner = device;
        ownerContext = context;
        presentIdentity = present;
        transport = candidate.release();
        activated = true;
        const auto& evidence = transport->evidence;
        LOG_INFO("DLSS-NR Dx11SceneStage: admitted NR VkQueue={:X} family={}, consumer VkQueue={:X} family={}, "
                 "Present VkQueue={:X} family={}; shared fences available",
                 evidence.nr, evidence.nrFamily, evidence.consumer, evidence.consumerFamily, evidence.present,
                 evidence.presentFamily);
    }
    std::array<ComPtr<ID3D11Texture2D>, 4> retained;
    std::array<ID3D11Texture2D*, 4> inputs {};
    const char* names[] = { NVSDK_NGX_Parameter_Output, NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_MotionVectors,
                            NVSDK_NGX_Parameter_ExposureTexture };
    for (unsigned i = 0; i < inputs.size(); ++i)
    {
        auto resource = Resource(params, names[i]);
        if ((resource && FAILED(resource->QueryInterface(IID_PPV_ARGS(&retained[i])))) || (!resource && i < 3))
        {
            Refuse("Dx11SceneStage: MissingOrUnsupportedInput");
            return;
        }
        inputs[i] = retained[i].Get();
    }
    SceneFrame call;
    auto& frame = call.frame;
    frame = GatherGuideFrame(params); // Scalars only; never reinterpret a D3D11 texture as D3D12.
    float pre = 1.0f;
    params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &pre);
    frame.PreExposure = std::isfinite(pre) && pre > 0.0f ? pre : 1.0f;
    frame.NativeDx11Scene = true;
    frame.Reset |= missed.exchange(false);
    frame.OutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    frame.DepthState = frame.MotionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // Only a full output is admitted in phase 3. Guides retain their own subrects and motion scales.
    D3D11_TEXTURE2D_DESC output {};
    inputs[0]->GetDesc(&output);
    unsigned baseX = 0, baseY = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, &baseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, &baseY);
    if (baseX || baseY || (frame.DisplayWidth && frame.DisplayWidth != output.Width) ||
        (frame.DisplayHeight && frame.DisplayHeight != output.Height))
    {
        Refuse("Dx11SceneStage: OutputSubrectUnsupported");
        return;
    }
    const auto result = transport->Run(inputs, Record, &call);
    if (result != Dx11Scene::Transport::Result::Recorded)
        missed = true;
    if (result == Dx11Scene::Transport::Result::Failed)
        Refuse(transport->Reason());
    else if (result == Dx11Scene::Transport::Result::Passthrough && !call.tracked)
        Refuse("Dx11SceneStage: SubmissionTrackingUnavailable");
    else
        status = transport->Reason();
}

const char* NativeDx11SceneStatus() { return Eligible() ? status.load() : nullptr; }

void ShutdownNativeDx11Scene()
{
    std::lock_guard lock(sceneMutex);
    if (!transport)
        return;
    refused = true; // One admitted owner per session, even after successful retirement.
    if (!transport->Retire())
    {
        status = "Dx11SceneStage: TeardownPendingRetained";
        LOG_INFO("DLSS-NR {} (no CPU wait, no release)", status.load());
        return;
    }
    delete transport;
    transport = nullptr;
    owner.Reset();
    ownerContext.Reset();
    status = "Dx11SceneStage: Retired";
}
} // namespace DlssNr
