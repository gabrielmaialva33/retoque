#include "pch.h"

#include "DlssNr_Dx12_Internal.h"
#include "DlssNr_GuideMatch_Dx12.h"
#include <dlssnr/DlssNr_ReplayClock.h>
#include <dlssnr/DlssNr_DriverInitGate.h>
#include <proxies/NVNGX_Proxy.h>
#include <State.h>
#include <Logger.h>
#include <algorithm>
#include <cmath>

namespace DlssNr::Dx12Internal
{
// Transfer 2's NGX entry points, as the driver core hands them out right now. Asked at every use rather than
// kept: a game that shuts NGX down (NVSDK_NGX_D3D12_Shutdown with OptiScaler's DLSS backend) turns them to
// null, and a null entry point is skipped rather than called into a core that is gone.
DlssNr::PrivateSr::Api PrivateSrApi()
{
    DlssNr::PrivateSr::Api api;
    api.allocate = NVNGXProxy::IsDx12Inited() ? NVNGXProxy::D3D12_AllocateParameters() : nullptr;
    api.destroy = NVNGXProxy::D3D12_DestroyParameters();
    api.create = NVNGXProxy::D3D12_CreateFeature();
    api.evaluate = NVNGXProxy::D3D12_EvaluateFeature();
    api.release = NVNGXProxy::D3D12_ReleaseFeature();
    return api;
}

bool NrEnlarger::CoreGone() const { return coreShutdowns != NVNGXProxy::Dx12Shutdowns(); }

NrEnlarger::~NrEnlarger()
{
    if (State::Instance().isShuttingDown || CoreGone())
        feature.Abandon();
    else
        feature.Release(PrivateSrApi());

    for (ID3D12Resource* resource : { carrier, enlarged, exposure, depth, motion })
    {
        if (resource != nullptr)
            resource->Release();
    }
}

// Published for the menu, and said in the log when the reason changes. A session that never selects 2 says
// nothing. A title whose frames alternate between two reasons would change it every frame, so after the first
// sixteen changes only every hundredth is said, with the count.
void PublishEnlarge(uint32_t transfer, DlssNr::Enlarge::Why why)
{
    using DlssNr::Enlarge::Why;
    static unsigned long long changes = 0;
    g_enlarge.transfer = transfer;
    const auto previous = static_cast<Why>(g_enlarge.why.exchange(static_cast<uint32_t>(why)));

    if (previous == why || (why == Why::NotSelected && previous == Why::NotYet))
        return;

    if (++changes <= 16 || changes % 100 == 0)
        LOG_INFO("DLSS-NR enlargement: {} (was {}); the resolve is sent Transfer {} ({} changes so far)",
                 DlssNr::Enlarge::Token(why), DlssNr::Enlarge::Token(previous), transfer, changes);
}

// What the resolve is sent this frame, decided before the timing contract is written so a sample says which
// enlargement it measured. Retires a bundle that is not this frame's -- another size, depth direction or
// device, or a feature the game took down with the NGX core -- or that this configuration no longer wants.
DlssNr::Enlarge::Decision PlanEnlarge(uint32_t configured, bool proxy, bool periphery, uint32_t debugView,
                                      const DlssNrFrameInfo& frame, ID3D12Device* device, bool beforeUpscale,
                                      bool guidesUsable, unsigned int width, unsigned int height,
                                      unsigned int workWidth, unsigned int workHeight, uint64_t present,
                                      uint64_t frameIndex)
{
    namespace Enlarge = DlssNr::Enlarge;
    const DlssNr::PrivateSr::Shape shape { workWidth, workHeight, width, height, frame.DepthInverted };

    if (g_enlarge.live != nullptr && g_enlarge.live->CoreGone())
        LOG_INFO("DLSS-NR enlargement: the game shut the NGX core down, and the private DLSS with it; it is "
                 "dropped without a call and built again");

    if (g_enlarge.live != nullptr &&
        (g_enlarge.live->feature.Built() != shape || g_enlarge.live->device != device || g_enlarge.live->CoreGone()))
        ParkNrEnlarger(g_enlarge.live);

    if (g_enlarge.live != nullptr && g_enlarge.live->guidesFailed)
        guidesUsable = false;

    auto state = Enlarge::State::Missing;

    if (g_enlarge.failed)
    {
        state = Enlarge::State::Failed;
    }
    else if (g_enlarge.live != nullptr)
    {
        const auto& live = *g_enlarge.live;
        const bool crossed = Enlarge::CreationCrossed(live.createdPresent, present, live.createdFrame, frameIndex) &&
                             (!live.tracked || DlssNr::Submission::Completed(live.creation));
        state = crossed ? Enlarge::State::Ready : Enlarge::State::Waiting;
    }

    Enlarge::Inputs in;
    in.configured = configured;
    in.proxy = proxy;
    in.periphery = periphery;
    in.width = width;
    in.height = height;
    in.workWidth = workWidth;
    in.workHeight = workHeight;
    in.beforeUpscale = beforeUpscale;
    in.realGuides = frame.AllowSupersampling;
    in.guidesUsable = guidesUsable;
    in.debugView = debugView;
    in.state = state;

    const auto decision = Enlarge::Decide(in);

    if (!decision.keep)
        ParkNrEnlarger(g_enlarge.live);

    PublishEnlarge(decision.transfer, decision.why);
    return decision;
}

// Internal type: EnlargeFrame (DlssNr_Dx12_Internal.h)

// Fails Transfer 2 for the session, until Retry: the bundle goes to the retirement list, and the resolve
// falls back to matched residual from this frame on.
void FailEnlarge(const char* what, unsigned int result)
{
    LOG_ERROR("DLSS-NR enlargement: {} (0x{:X} {}); matched residual from here, until Retry", what, result,
              NgxResultName(result));
    g_enlarge.failed = true;
    ParkNrEnlarger(g_enlarge.live);
    PublishEnlarge(DlssNr::Enlarge::kTransferMatched, DlssNr::Enlarge::Why::Failed);
}

// Builds the private DLSS on cmdList. It never evaluates here: creating and evaluating in one command list is
// the dice-roll that hung the GPU when the model did it. The first evaluation waits for CreationCrossed.
void BuildEnlarger(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, const EnlargeFrame& f)
{
    // The cap on parked objects is a refusal to allocate, not a failure: try again on a later frame.
    if (g_tracked && !RetiredCapacity())
        return;

    // A core the game has shut down is waited for, not failed: a game that applies settings starts it again,
    // and the bundle is built on the next frame that finds it up. A core that is up and lacks an entry point
    // is a failure.
    if (!NVNGXProxy::IsDx12Inited())
    {
        PublishEnlarge(DlssNr::Enlarge::kTransferMatched, DlssNr::Enlarge::Why::WarmingUp);
        return;
    }

    const auto api = PrivateSrApi();

    if (!api.Complete())
    {
        FailEnlarge("the NGX core does not offer the five entry points it needs",
                    (unsigned int) NVSDK_NGX_Result_FAIL_NotInitialized);
        return;
    }

    auto* bundle = new NrEnlarger();
    bundle->device = device;
    bundle->coreShutdowns = NVNGXProxy::Dx12Shutdowns();

    // Tracked before anything is allocated: a recording the tracker refuses is not a creation, and leaves
    // nothing behind to retire (it is asked again on the next frame).
    if (g_tracked)
    {
        bundle->tracked = true;

        if (!DlssNr::Submission::Track(cmdList, bundle->creation))
        {
            delete bundle;
            return;
        }
    }

    bundle->carrier = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, f.workWidth, f.workHeight);
    bundle->enlarged = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, f.width, f.height);
    bundle->exposure = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, 1, 1);

    if (bundle->carrier == nullptr || bundle->enlarged == nullptr || bundle->exposure == nullptr)
    {
        ParkNrEnlarger(bundle);
        FailEnlarge("its textures could not be created", (unsigned int) NVSDK_NGX_Result_FAIL_OutOfGPUMemory);
        return;
    }

    const DlssNr::PrivateSr::Shape shape { f.workWidth, f.workHeight, f.width, f.height, f.depthInverted };
    NVSDK_NGX_Result created = NVSDK_NGX_Result_Fail;
    {
        DlssNr::DriverInitGate::Scope gate(DlssNr::DriverInitGate::Hold::Ngx, true,
                                           "the enlargement's DLSS feature creation");
        created = bundle->feature.Create(api, cmdList, shape);
    }

    if (created != NVSDK_NGX_Result_Success)
    {
        ParkNrEnlarger(bundle);
        FailEnlarge("the private DLSS Super Resolution would not create", (unsigned int) created);
        return;
    }

    bundle->createdPresent = f.present;
    bundle->createdFrame = f.frame;
    g_enlarge.live = bundle;

    LOG_INFO("DLSS-NR enlargement: private DLSS Super Resolution created, {}x{} -> {}x{}, depth {}, LDR, "
             "MaxQuality; it runs from a later frame",
             f.workWidth, f.workHeight, f.width, f.height, f.depthInverted ? "inverted" : "not inverted");
}

// Runs the private DLSS over the edit when the plan says so, or builds it on the frame the plan says to.
// Returns its output, readable, for the resolve to decode; null sends the resolve matched residual this frame.
// The caller moves the output back to UNORDERED_ACCESS after the resolve.
ID3D12Resource* EnlargeEdit(DlssNr_Dx12& pass, ID3D12GraphicsCommandList* cmdList, ID3D12Device* device,
                            const DlssNr::Enlarge::Decision& plan, const EnlargeFrame& f)
{
    namespace Enlarge = DlssNr::Enlarge;

    if (plan.build && g_enlarge.live == nullptr)
    {
        BuildEnlarger(cmdList, device, f);
        return nullptr;
    }

    if (!plan.evaluate || g_enlarge.live == nullptr)
        return nullptr;

    NrEnlarger& e = *g_enlarge.live;

    // Depth and motion at the working size: the model's pair, the game's when it already is that size, or the
    // guide resample into the bundle's own pair.
    const auto source =
        Enlarge::Guides(f.matchedDepth != nullptr && f.matchedMotion != nullptr, DlssNr_GuideMatch_Dx12::Available(),
                        f.workWidth, f.workHeight, f.depthBaseX, f.depthBaseY, f.depthWidth, f.depthHeight,
                        f.motionBaseX, f.motionBaseY, f.motionWidth, f.motionHeight);
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    bool resampled = false;

    if (source == Enlarge::GuideSource::Matched)
    {
        depth = f.matchedDepth;
        motion = f.matchedMotion;
    }
    else if (source == Enlarge::GuideSource::Game)
    {
        depth = f.gameDepth;
        motion = f.gameMotion;
    }
    else if (source == Enlarge::GuideSource::Resample)
    {
        if (g_nr.guideMatch == nullptr)
            g_nr.guideMatch = new DlssNr_GuideMatch_Dx12("DLSS-NR guide match", device);
        if (e.depth == nullptr)
            e.depth = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, f.workWidth, f.workHeight);
        if (e.motion == nullptr)
            e.motion = CreateScratch(device, DXGI_FORMAT_R32G32_FLOAT, f.workWidth, f.workHeight);

        if (g_nr.guideMatch->IsInit() && e.depth != nullptr && e.motion != nullptr &&
            g_nr.guideMatch->Dispatch(cmdList, f.gameDepth, f.gameMotion, f.depthBaseX, f.depthBaseY, f.depthWidth,
                                      f.depthHeight, f.motionBaseX, f.motionBaseY, f.motionWidth, f.motionHeight,
                                      e.depth, e.motion, f.workWidth, f.workHeight))
        {
            Barrier(cmdList, e.depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmdList, e.motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            depth = e.depth;
            motion = e.motion;
            resampled = true;
        }
    }

    if (depth == nullptr || motion == nullptr)
    {
        // Not a failure of DLSS: the guides could not be brought to the working size. Held on the bundle, so the
        // plan says so from the next frame until it is rebuilt (a resize, Retry) instead of planning a run that
        // falls back every frame. Its history starts over when it next runs.
        e.guidesFailed = true;
        PublishEnlarge(Enlarge::kTransferMatched, Enlarge::Why::GuidesUnmatched);
        return nullptr;
    }

    // The carrier, and the exposure of 1 DLSS is handed, rewritten every time rather than trusted from the
    // creation frame: one texel, and nothing then depends on that frame's list having been submitted.
    DlssNrConstants carrier {};
    carrier.Mode = DlssNrMode_Carrier;
    carrier.Width = f.workWidth;
    carrier.Height = f.workHeight;
    carrier.Passthrough = f.passthrough ? 1u : 0u;

    DlssNrConstants unit {};
    unit.Mode = DlssNrMode_UnitTexel;
    unit.Width = 1;
    unit.Height = 1;

    const bool prepared =
        pass.DispatchPass(cmdList, carrier, f.proxy, f.answer, nullptr, nullptr, nullptr, e.carrier, nullptr) &&
        pass.DispatchPass(cmdList, unit, f.proxy, nullptr, nullptr, nullptr, nullptr, e.exposure, nullptr);

    Barrier(cmdList, e.carrier, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, e.exposure, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // The frame time handed to the enlargement model: recorded time under an offline replay (offline-replay-seam.md).
    const auto now = DlssNr::ReplayClock::Now();
    const float sinceLast = std::chrono::duration<float, std::milli>(now - e.lastTime).count();

    DlssNr::PrivateSr::Frame sr;
    sr.color = e.carrier;
    sr.output = e.enlarged;
    sr.depth = depth;
    sr.motion = motion;
    sr.exposure = e.exposure;
    sr.width = f.workWidth;
    sr.height = f.workHeight;
    sr.reset = Enlarge::ResetHistory(f.reset, false, f.hold != g_enlarge.lastHold, f.frame, e.lastFrame);
    sr.mvScaleX = f.mvScaleX;
    sr.mvScaleY = f.mvScaleY;
    sr.frameTimeMs =
        e.lastFrame != 0 && std::isfinite(sinceLast) ? std::clamp(sinceLast, 1.0f, 100.0f) : 1000.0f / 60.0f;

    const auto result = prepared ? e.feature.Evaluate(PrivateSrApi(), cmdList, sr) : NVSDK_NGX_Result_Fail;

    Barrier(cmdList, e.carrier, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmdList, e.exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (resampled)
    {
        Barrier(cmdList, e.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmdList, e.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    g_enlarge.lastHold = f.hold;

    if (result != NVSDK_NGX_Result_Success)
    {
        FailEnlarge(prepared ? "the private DLSS Super Resolution refused to evaluate"
                             : "the carrier could not be recorded",
                    (unsigned int) result);
        return nullptr;
    }

    if (e.lastFrame == 0)
        LOG_INFO("DLSS-NR enlargement: first private DLSS evaluation, {}x{} -> {}x{}, guides {}, motion scale "
                 "{:.1f} x {:.1f}",
                 f.workWidth, f.workHeight, f.width, f.height,
                 source == Enlarge::GuideSource::Matched ? "the model's matched pair"
                 : source == Enlarge::GuideSource::Game  ? "the game's own"
                                                         : "resampled for it",
                 f.mvScaleX, f.mvScaleY);

    e.lastFrame = f.frame;
    e.lastTime = now;

    Barrier(cmdList, e.enlarged, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return e.enlarged;
}

} // namespace DlssNr::Dx12Internal
