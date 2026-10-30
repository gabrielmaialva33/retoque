#include "pch.h"

#include "DlssNr_Dx12_Internal.h"

#include <dlssnr/DlssNr.h>

#include <dlssnr/DlssNr_Capture.h>
#include <dlssnr/DlssNr_PairCaptureDx12.h>
#include <dlssnr/DlssNr_Proxy.h>
#include <dlssnr/DlssNr_ExposureScan.h>
#include <dlssnr/DlssNr_Replay.h>
#include <dlssnr/DlssNr_ReplayClock.h>
#include <dlssnr/DlssNr_ReplayRecorderDx12.h>
#include <dlssnr/DlssNr_Exposure.h>
#include <dlssnr/DlssNr_Chain.h>
#include <dlssnr/DlssNr_Consumers.h>
#include <dlssnr/DlssNr_Submission.h>
#include <dlssnr/DlssNr_ModelLog.h>
#include <dlssnr/DlssNr_Session.h>
#include <dlssnr/DlssNr_Dred.h>
#include <dlssnr/DlssNr_PresentBoundary.h>
#include <dlssnr/DlssNr_SlBoundaryProbe.h>
#include <dlssnr/DlssNr_DirectRuntime.h>
#include <dlssnr/DlssNr_NgxInfo.h>
#include <dlssnr/DlssNr_PresentColour.h>
#include <dlssnr/DlssNr_DisplayWhite.h>
#include <dlssnr/DlssNr_SwapchainColour.h>
#include <dlssnr/DlssNr_Enlarge.h>
#include <dlssnr/DlssNr_PrivateSr.h>
#include <dxgi1_4.h>

#include "DlssNr_Dx12.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <proxies/NVNGX_Proxy.h>
#include <hooks/D3D12_Hooks.h>
#include <dlssnr/DlssNr_GpuTiming.h>
#include <dlssnr/DlssNr_LogRate.h>
#include <dlssnr/DlssNr_ZeroGuides.h>
#include <dlssnr/DlssNr_RenderSize.h>
#include <dlssnr/DlssNr_WorkingScale.h>
#include <shaders/synth_motion/SynthMotion_Dx12.h>
#include <shaders/synth_motion/SynthMotion_Handoff.h>
#include <shaders/synth_motion/SynthMotionNvofa_Dx12.h>

#include <mutex>
#include <optional>
#include <tuple>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include "precompile/DlssNr_Shader.h"
#include <shaders/output_scaling/OS_Dx12.h>
#include "DlssNr_Stabilizer_Dx12.h"
#include "DlssNr_UiMask_Dx12.h"
#include "DlssNr_GuideMatch.h"
#include "DlssNr_GuideMatch_Dx12.h"
#include "DlssNr_Cadence_Dx12.h"
#include <dlssnr/DlssNr_Cadence.h>
#include <dlssnr/DlssNr_PresentSync.h>
#include <proxies/KernelBase_Proxy.h>
#include <framegen/IFGFeature_Dx12.h>
#include "DlssNr_Periphery.h"
#include "DlssNr_Periphery_Dx12.h"
#include <dlssnr/DlssNr_KernelProfile.h>
#include "DlssNr_DetailStats_Dx12.h"
#include <dlssnr/DlssNr_DetailStats.h>
#include "DlssNr_Pulse_Dx12.h"
#include "DlssNr_MotionProbe_Dx12.h"
#include "DlssNr_PresentHdr_Dx12.h"
#include <dlssnr/DlssNr_Pulse.h>
#include <dlssnr/DlssNr_PresentGuideRing.h>
#include <menu/menu_overlay_base.h>
#include <dlssnr/DlssNr_PresentGuideIdentity.h>

using namespace DlssNr::Dx12Internal;

namespace DlssNr
{
// Creates a dedicated guide clone for the present snapshot ring matching the donor description
// (present_path.hpp:1018-1035): exact source format and flags preserved, Alignment = 0, MipLevels = 1, DepthOrArraySize
// = 1, SampleDesc.Count = 1.
namespace Dx12Internal
{
static ID3D12Resource* CreatePresentGuideClone(ID3D12Device* device, ID3D12Resource* source)
{
    if (!DeviceChildBelongsTo(device, source))
        return nullptr;

    const D3D12_RESOURCE_DESC src = source->GetDesc();
    if (src.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || src.DepthOrArraySize != 1 || src.SampleDesc.Count != 1)
        return nullptr;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = src;
    desc.Alignment = 0;
    desc.MipLevels = 1;
    // As present_path.hpp:1029: a plain layout, and no flag that would stop the copy being read (DENY_SHADER_RESOURCE)
    // or let it decay to COMMON between submissions (ALLOW_SIMULTANEOUS_ACCESS). The reuse check compares the
    // stored source flags, so this does not rebuild the clone every frame.
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags &= ~(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
    desc.DepthOrArraySize = 1;
    desc.SampleDesc.Count = 1;

    if (!PresentGuideCopyCompatible(device, src, desc))
        return nullptr;

    ID3D12Resource* res = nullptr;
    const HRESULT hr =
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&res));

    if (SUCCEEDED(hr) && res != nullptr)
        return res;

    return nullptr;
}
} // namespace Dx12Internal

// What an evaluate says about the picture its guides belong to: the device their copies live on, and the output's
// extent and base. The extent is the caller's, read from the output resource of whichever API made it (a D3D12
// evaluate's D3D12OutputExtent, a native D3D11 one's own texture); the base is the evaluate's
// DLSS.Output.Subrect.Base, read the same way for both.
namespace Dx12Internal
{
static DlssNr::PresentGuideIdentity::Stamp EvaluateOutput(NVSDK_NGX_Parameter* params, const void* device,
                                                          uint32_t outputWidth, uint32_t outputHeight)
{
    DlssNr::PresentGuideIdentity::Stamp stamp;
    stamp.device = device;
    stamp.outputWidth = outputWidth;
    stamp.outputHeight = outputHeight;

    params->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, &stamp.outputBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, &stamp.outputBaseY);
    return stamp;
}
} // namespace Dx12Internal

// A D3D12 evaluate's output extent: its Output resource, or zero when it named none.
namespace Dx12Internal
{
static void D3D12OutputExtent(NVSDK_NGX_Parameter* params, uint32_t* width, uint32_t* height)
{
    *width = 0;
    *height = 0;

    if (ID3D12Resource* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output"))
    {
        const auto desc = output->GetDesc();
        *width = static_cast<uint32_t>(desc.Width);
        *height = desc.Height;
    }
}
} // namespace Dx12Internal

// The stamp, at capture: the evaluate's output (EvaluateOutput), and how much of the guides is real.
namespace Dx12Internal
{
static void StampPresentGuide(size_t slot, DlssNr::PresentGuideIdentity::Stamp stamp, const DlssNrFrameInfo& frame,
                              uint32_t depthWidth, uint32_t depthHeight)
{
    const auto render = DlssNr::PresentGuideIdentity::RenderExtent(frame.RenderSubrectWidth, frame.RenderSubrectHeight,
                                                                   depthWidth, depthHeight);
    stamp.renderWidth = render.width;
    stamp.renderHeight = render.height;

    g_guideIdentity.tracker.StampCapture(slot, stamp, NowMs());
}
} // namespace Dx12Internal

namespace Dx12Internal
{
static void NoteBridgePresentedExtent(unsigned int width, unsigned int height)
{
    if (width != 0 && height != 0)
        g_bridgePresentedExtent = static_cast<uint64_t>(width) << 32 | height;
}
} // namespace Dx12Internal

// Who presents, for rule 3: the swapchain's window, so that the game's Streamline proxy at the boundary and the
// swapchain beneath it (the wrapped entry) are one presenter, not two that would refuse each other's captures. The
// swapchain itself when it has no window. Called before the lock: a call into the swapchain under it is not needed.
namespace Dx12Internal
{
const void* PresentGuidePresenter(IDXGISwapChain3* swapchain)
{
    HWND window = nullptr;

    if (swapchain != nullptr && SUCCEEDED(swapchain->GetHwnd(&window)) && window != nullptr)
        return window;

    return swapchain;
}
} // namespace Dx12Internal

// GuideAsk is declared in DlssNr_Dx12_Internal.h.

// The rules over this present's pick (D2): the answer's slot is the pick or none. Marks a frame boundary and notes
// the presenter. A ring that never recorded -- a title with no upscaler -- is not asked: no boundary, no presenter.
namespace Dx12Internal
{
static GuideAsk AskGuideIdentity(const DlssNr::PresentGuideIdentity::Consumer& consumer, const void* device,
                                 const void* presenter, unsigned int width, unsigned int height, size_t picked,
                                 bool enforceLag)
{
    GuideAsk ask;

    if (g_presentGuideRing.NewestRecordedSequence() == 0)
        return ask;

    ask.asked = true;
    ask.present.device = device;
    ask.present.width = width;
    ask.present.height = height;
    ask.present.swapchain = presenter;
    ask.present.lastConsumed = consumer.LastConsumed();
    ask.present.nowMs = NowMs();
    ask.present.enforceLag = enforceLag;
    ask.decision = g_guideIdentity.tracker.AtPresent(picked, g_presentGuideRing, ask.present);
    return ask;
}
} // namespace Dx12Internal

// What the present did with the answer: in the consumer's window (a line when one is due) when `logged`, and in the
// status, published once a second. A use moves rule 5's record whether it is logged or not: a host that is not logged
// may still be running the pass (a bridge host built at HookMethod=2 keeps going after it changes).
namespace Dx12Internal
{
static void CountGuideIdentity(DlssNr::PresentGuideIdentity::Consumer& consumer, const GuideAsk& ask, size_t picked,
                               DlssNr::PresentGuideIdentity::Outcome outcome, bool logged)
{
    // The steady clock, through NowMs, which reads it, so the windows run on the clock the rest of the glue uses.
    const DlssNr::PresentGuideIdentity::Clock::time_point now { std::chrono::milliseconds(NowMs()) };

    std::string line;
    if (ask.asked && logged &&
        consumer.Note(ask.decision, picked, g_presentGuideRing, g_guideIdentity.tracker.Stamped(), ask.present, outcome,
                      now, &line))
    {
        LOG_INFO("{}", line);
    }
    else if (ask.asked && !logged && outcome == DlssNr::PresentGuideIdentity::Outcome::Used)
    {
        consumer.MarkUsed(g_presentGuideRing.GetSlot(picked).sequence);
    }

    DlssNr::PresentGuideIdentity::Status second;
    if (g_guideIdentity.status.Add(outcome, ask.decision.reason, now, &second))
    {
        g_guideStatus = DlssNr::PresentGuideIdentity::Pack(second);
        g_guideStatusAtMs = NowMs();
    }
}
} // namespace Dx12Internal

// A change of guide source that has held for kSourceStable presents drops the model's history once (D3), through the
// same request a route switch makes. Said the first three times and then every hundredth. Every present whose pass
// runs is noted, a ring that never recorded included: its frames are the zero or synthesized side, so the change to
// the game's guides when captures start is a change like any other, and a title that never captures never changes.
// The answer: whether the game's guides still hold the history after this present, which a single miss does not
// change (the ZeroGuideReset policy waits for that too).
namespace Dx12Internal
{
static bool NoteGuideSource(DlssNr::PresentGuideIdentity::SourceHistory& history, bool game, const char* consumer)
{
    if (!history.Note(game))
        return history.Established() == 1;

    g_resetOnReturn = true;

    static unsigned long long resets = 0;
    if (++resets <= 3 || resets % 100 == 0)
    {
        LOG_INFO("{}", std::string("DLSS-NR present guides, ") + consumer + ": the model's history restarts on " +
                           (game ? "the game's depth and motion" : "zero or synthesized guides") + ", held for " +
                           std::to_string(DlssNr::PresentGuideIdentity::kSourceStable) + " frames (" +
                           std::to_string(resets) + " so far)");
    }

    return history.Established() == 1;
}
} // namespace Dx12Internal

// The D3D12 present pass, right after its pick. Rule 6 is enforced at the game's own Present only (`boundary`).
namespace Dx12Internal
{
GuideAsk AskPresentPassGuides(ID3D12Device* device, const void* presenter, unsigned int width, unsigned int height,
                              size_t picked, bool boundary)
{
    return AskGuideIdentity(g_guideIdentity.present, device, presenter, width, height, picked, boundary);
}
} // namespace Dx12Internal

namespace Dx12Internal
{
void CountPresentPassGuides(const GuideAsk& ask, size_t picked, DlssNr::PresentGuideIdentity::Outcome outcome)
{
    CountGuideIdentity(g_guideIdentity.present, ask, picked, outcome, true);
}
} // namespace Dx12Internal

// For a present whose pass runs. `routeStarts`: the first present on this route since the upscaled one ran, which
// resets the model on its own; the source is counted from there. True while the game's guides hold the history.
namespace Dx12Internal
{
bool NotePresentPassGuideSource(bool game, bool routeStarts)
{
    if (routeStarts)
        g_guideIdentity.presentSource.Reset();

    return NoteGuideSource(g_guideIdentity.presentSource, game, "D3D12 present");
}
} // namespace Dx12Internal

// The D3D12 pass under RequireDlss, on a frame with no guides the rules accept: shown as the game drew it.
namespace Dx12Internal
{
void NotePresentGuideWaiting()
{
    g_guideFallback = static_cast<uint8_t>(DlssNr::PresentGuideIdentity::Fallback::Waiting);
}
} // namespace Dx12Internal

// The D3D11 bridge's present host, right after its pick in CapturedPresentGuides. A host once built keeps asking after
// the pass is switched off or HookMethod leaves 2, the only value the bridge is built for; the answer still holds
// then, but the present is not logged. Rule 6 is not enforced here: nothing has measured this route.
namespace Dx12Internal
{
static GuideAsk AskBridgeGuides(ID3D12GraphicsCommandList* cmdList, const void* presenter, unsigned int width,
                                unsigned int height, size_t picked)
{
    // A ring that never recorded (Divinity, any bridge title with no upscaler): nothing to ask, so not even the list.
    if (g_presentGuideRing.NewestRecordedSequence() == 0)
        return {};

    // The device the bridge's list reads the copies on. Compared as an identity only; the list keeps it alive. A
    // list with no device is asked with none, which rule 1 refuses.
    ID3D12Device* device = nullptr;
    if (cmdList != nullptr && SUCCEEDED(cmdList->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
        device->Release();
    else
        device = nullptr;

    return AskGuideIdentity(g_guideIdentity.bridge, device, presenter, width, height, picked, false);
}
} // namespace Dx12Internal

// True while the game's guides hold the bridge's history.
namespace Dx12Internal
{
static bool CountBridgeGuides(const GuideAsk& ask, size_t picked, DlssNr::PresentGuideIdentity::Outcome outcome,
                              const void* presenter)
{
    const Config& cfg = *Config::Instance();
    const bool logged =
        presenter != nullptr && cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrHookMethod.value_or_default() == 2;

    CountGuideIdentity(g_guideIdentity.bridge, ask, picked, outcome, logged);
    return NoteGuideSource(g_guideIdentity.bridgeSource, outcome == DlssNr::PresentGuideIdentity::Outcome::Used,
                           "D3D11 bridge");
}
} // namespace Dx12Internal

void NotePresentGuideFallback(bool synthesized)
{
    using DlssNr::PresentGuideIdentity::Fallback;
    g_guideFallback = static_cast<uint8_t>(synthesized ? Fallback::Synthesized : Fallback::Zero);
}

// The last second of guide outcomes, when a present host published one lately: the present route is running.
namespace Dx12Internal
{
bool RecentGuideStatus(DlssNr::PresentGuideIdentity::Status* status)
{
    const long long at = g_guideStatusAtMs.load();

    if (at == 0 || NowMs() - at > 2000)
        return false;

    *status = DlssNr::PresentGuideIdentity::Unpack(g_guideStatus.load());
    return true;
}
} // namespace Dx12Internal

namespace Dx12Internal
{
static DlssNr::PresentGuideIdentity::Fallback RecentGuideFallback()
{
    return static_cast<DlssNr::PresentGuideIdentity::Fallback>(g_guideFallback.load());
}
} // namespace Dx12Internal

namespace Dx12Internal
{
DlssNr::PresentGuideIdentity::Source RecentGuideSource(const DlssNr::PresentGuideIdentity::Status& status)
{
    return DlssNr::PresentGuideIdentity::SourceOf(status, RecentGuideFallback());
}
} // namespace Dx12Internal

std::string PresentGuidesLine()
{
    DlssNr::PresentGuideIdentity::Status status;

    if (!Config::Instance()->DlssNrEnabled.value_or_default() || !RecentGuideStatus(&status))
        return {};

    return DlssNr::PresentGuideIdentity::GuidesLine(status, RecentGuideFallback());
}

// The guides one capture copies into the present guide ring, wherever they came from: a D3D12 evaluate's own
// resources (CaptureTemporal), or the bridge's D3D12 side of a native D3D11 evaluate's (CaptureBridgedGuides). The
// states are where each source rests on the list, before and after the copy. `output` is EvaluateOutput's, without
// its device, which is the depth's.
namespace Dx12Internal
{
struct GuideCapture
{
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    DlssNr::PresentGuideIdentity::Stamp output {};
};
} // namespace Dx12Internal

// The capture: a ring slot, its clones (rebuilt for another device, format, size or flags, the old ones parked, never
// freed under the GPU), the copies recorded on cmdList and tracked before anything is recorded, the stamp, the commit.
// True when committed. Takes g_nrMutex.
namespace Dx12Internal
{
static bool CaptureGuides(ID3D12GraphicsCommandList* cmdList, const GuideCapture& source, const DlssNrFrameInfo& frame)
{
    if (g_nr.failed || cmdList == nullptr || source.depth == nullptr || source.motion == nullptr)
        return false;

    ID3D12Resource* depth = source.depth;
    ID3D12Resource* motion = source.motion;

    ID3D12Device* device = nullptr;
    if (FAILED(depth->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return false;

    if (!DeviceChildBelongsTo(device, cmdList) || !DeviceChildBelongsTo(device, motion))
    {
        device->Release();
        return false;
    }

    const auto depthDesc = depth->GetDesc();
    const auto motionDesc = motion->GetDesc();

    if (depthDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || depthDesc.DepthOrArraySize != 1 ||
        depthDesc.SampleDesc.Count != 1 || motionDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        motionDesc.DepthOrArraySize != 1 || motionDesc.SampleDesc.Count != 1)
    {
        device->Release();
        return false;
    }

    auto depthCopyDesc = depthDesc;
    auto motionCopyDesc = motionDesc;
    depthCopyDesc.MipLevels = motionCopyDesc.MipLevels = 1;
    if (!PresentGuideCopyCompatible(device, depthDesc, depthCopyDesc) ||
        !PresentGuideCopyCompatible(device, motionDesc, motionCopyDesc))
    {
        device->Release();
        return false;
    }

    struct AllocFailKey
    {
        const void* device;
        DXGI_FORMAT format;
        uint64_t width;
        uint32_t height;
        auto operator<=>(const AllocFailKey&) const = default;
    };
    static std::set<AllocFailKey> s_failedAllocs;

    const void* const devId = static_cast<const void*>(device);

    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    TickNrRetired();

    if (s_failedAllocs.contains({ devId, depthDesc.Format, depthDesc.Width, depthDesc.Height }) ||
        s_failedAllocs.contains({ devId, motionDesc.Format, motionDesc.Width, motionDesc.Height }))
    {
        device->Release();
        return false;
    }

    const auto isReadInFlight = [](void* token) -> bool
    {
        if (token == nullptr)
            return false;
        auto* c = static_cast<PresentGuideClone*>(token);
        if (!c->readTracked)
            return false;
        if (DlssNr::Submission::Completed(c->presentReadUsage))
        {
            c->readTracked = false;
            return false;
        }
        return true;
    };

    const size_t slot = g_presentGuideRing.AcquireForRecord(isReadInFlight);
    if (slot >= DlssNr::PresentGuides::kRingSize)
    {
        device->Release();
        return false;
    }

    auto& clone = g_presentGuideClones[slot];

    if (clone.depth != nullptr)
    {
        if (clone.deviceIdentity != devId || clone.sourceDepthFormat != depthDesc.Format ||
            clone.sourceDepthWidth != depthDesc.Width || clone.sourceDepthHeight != depthDesc.Height ||
            clone.sourceDepthFlags != depthDesc.Flags)
        {
            ParkNrClone(clone.depth, clone.writeUsage, clone.tracked);
        }
    }
    if (clone.motion != nullptr)
    {
        if (clone.deviceIdentity != devId || clone.sourceMotionFormat != motionDesc.Format ||
            clone.sourceMotionWidth != motionDesc.Width || clone.sourceMotionHeight != motionDesc.Height ||
            clone.sourceMotionFlags != motionDesc.Flags)
        {
            ParkNrClone(clone.motion, clone.writeUsage, clone.tracked);
        }
    }

    if (clone.depth == nullptr)
        clone.depth = CreatePresentGuideClone(device, depth);
    if (clone.motion == nullptr)
        clone.motion = CreatePresentGuideClone(device, motion);

    clone.deviceIdentity = devId;
    const bool copyCompatible = clone.depth != nullptr && clone.motion != nullptr &&
                                DeviceChildBelongsTo(device, clone.depth) &&
                                DeviceChildBelongsTo(device, clone.motion) &&
                                PresentGuideCopyCompatible(device, depthDesc, clone.depth->GetDesc()) &&
                                PresentGuideCopyCompatible(device, motionDesc, clone.motion->GetDesc());
    device->Release();
    if (clone.depth != nullptr && clone.motion != nullptr && !copyCompatible)
    {
        g_presentGuideRing.InvalidateSlot(slot);
        return false;
    }

    if (clone.depth == nullptr || clone.motion == nullptr)
    {
        if (clone.depth == nullptr)
            s_failedAllocs.insert({ devId, depthDesc.Format, depthDesc.Width, depthDesc.Height });
        else
            ParkNrClone(clone.depth, clone.writeUsage, clone.tracked);

        if (clone.motion == nullptr)
            s_failedAllocs.insert({ devId, motionDesc.Format, motionDesc.Width, motionDesc.Height });
        else
            ParkNrClone(clone.motion, clone.writeUsage, clone.tracked);

        LOG_ERROR("DLSS-NR present guides: clone allocation failed on device {:p}; present route will use neutral "
                  "guides",
                  devId);
        g_presentGuideRing.InvalidateSlot(slot);
        return false;
    }

    clone.sourceDepthFormat = depthDesc.Format;
    clone.sourceDepthWidth = depthDesc.Width;
    clone.sourceDepthHeight = depthDesc.Height;
    clone.sourceDepthFlags = depthDesc.Flags;

    clone.sourceMotionFormat = motionDesc.Format;
    clone.sourceMotionWidth = motionDesc.Width;
    clone.sourceMotionHeight = motionDesc.Height;
    clone.sourceMotionFlags = motionDesc.Flags;

    // Submission tracking before ANY barrier or recording on this list (Correction 4)
    clone.captureWriteUsage = {}; // reset single capture write usage for eligibility (Correction 2)
    const bool trackedNow = DlssNr::Submission::Track(cmdList, clone.writeUsage) &&
                            DlssNr::Submission::Track(cmdList, clone.captureWriteUsage);
    // Never set back to false: earlier captures' writes may still be pending on the GPU.
    clone.tracked = clone.tracked || trackedNow;
    if (!trackedNow)
    {
        static bool s_loggedTrackFail = false;
        if (!s_loggedTrackFail)
        {
            LOG_WARN("DLSS-NR present guides: evaluate command list refused submission tracking (type != DIRECT or "
                     "sealed); skipping guide snapshot");
            s_loggedTrackFail = true;
        }
        g_presentGuideRing.InvalidateSlot(slot);
        return false;
    }

    // Transitions around copy: game guide to COPY_SOURCE, clone to COPY_DEST, copy mip 0, restore states
    const auto depthArrival = source.depthState;
    const auto motionArrival = source.motionState;

    if (depthArrival != D3D12_RESOURCE_STATE_COPY_SOURCE)
        Barrier(cmdList, depth, depthArrival, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmdList, clone.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    const CD3DX12_TEXTURE_COPY_LOCATION dstDepth(clone.depth, 0);
    const CD3DX12_TEXTURE_COPY_LOCATION srcDepth(depth, 0);
    cmdList->CopyTextureRegion(&dstDepth, 0, 0, 0, &srcDepth, nullptr);

    Barrier(cmdList, clone.depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (depthArrival != D3D12_RESOURCE_STATE_COPY_SOURCE)
        Barrier(cmdList, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, depthArrival);

    if (motionArrival != D3D12_RESOURCE_STATE_COPY_SOURCE)
        Barrier(cmdList, motion, motionArrival, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmdList, clone.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    const CD3DX12_TEXTURE_COPY_LOCATION dstMotion(clone.motion, 0);
    const CD3DX12_TEXTURE_COPY_LOCATION srcMotion(motion, 0);
    cmdList->CopyTextureRegion(&dstMotion, 0, 0, 0, &srcMotion, nullptr);

    Barrier(cmdList, clone.motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (motionArrival != D3D12_RESOURCE_STATE_COPY_SOURCE)
        Barrier(cmdList, motion, D3D12_RESOURCE_STATE_COPY_SOURCE, motionArrival);

    clone.frame = frame;
    clone.frame.DepthState = (int) D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    clone.frame.MotionState = (int) D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    clone.width = static_cast<uint32_t>(depthDesc.Width);
    clone.height = depthDesc.Height;

    ++g_renderSeq; // Count render frame at successful CaptureTemporal (Correction 5b)

    DlssNr::PresentGuideIdentity::Stamp stamp = source.output;
    stamp.device = devId;
    StampPresentGuide(slot, stamp, frame, clone.width, clone.height);
    g_presentGuideRing.CommitRecord(slot, clone.width, clone.height, frame.DepthInverted, NowMs(),
                                    &clone.captureWriteUsage, &clone);
    g_temporalValid = true;
    return true;
}
} // namespace Dx12Internal

// A D3D12 evaluate's capture: its own depth and motion, in the states [OptiScaler] DepthResourceBarrier and
// MVResourceBarrier say the game leaves them in, and its own output.
namespace Dx12Internal
{
void CaptureTemporal(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, const DlssNrFrameInfo& frame)
{
    if (g_nr.failed)
        return;

    GuideCapture source;
    source.depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    source.motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    if (source.depth == nullptr || source.motion == nullptr || cmdList == nullptr)
        return;

    const auto& cfg = *Config::Instance();
    source.depthState = static_cast<D3D12_RESOURCE_STATES>(
        cfg.DepthResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    source.motionState = static_cast<D3D12_RESOURCE_STATES>(
        cfg.MVResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    D3D12OutputExtent(params, &outputWidth, &outputHeight);
    source.output = EvaluateOutput(params, nullptr, outputWidth, outputHeight);

    CaptureGuides(cmdList, source, frame);
}
} // namespace Dx12Internal

bool CaptureBridgedGuides(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* depth, ID3D12Resource* motion,
                          D3D12_RESOURCE_STATES state, NVSDK_NGX_Parameter* params, uint32_t outputWidth,
                          uint32_t outputHeight)
{
    if (params == nullptr)
        return false;

    GuideCapture source;
    source.depth = depth;
    source.motion = motion;
    source.depthState = state;
    source.motionState = state;
    source.output = EvaluateOutput(params, nullptr, outputWidth, outputHeight);

    return CaptureGuides(cmdList, source, GatherGuideFrame(params));
}

unsigned long long BridgePresentedExtent() { return g_bridgePresentedExtent.load(); }

// The bridge's read of one slot, under g_nrMutex: tracked before it is handed out, or not handed out at all.
namespace Dx12Internal
{
static bool ReadCapturedGuides(size_t slot, ID3D12GraphicsCommandList* cmdList, ID3D12Resource** depth,
                               ID3D12Resource** motion, DlssNrFrameInfo* frame)
{
    if (slot >= DlssNr::PresentGuides::kRingSize)
        return false;

    auto& clone = g_presentGuideClones[slot];
    if (clone.depth == nullptr || clone.motion == nullptr || cmdList == nullptr)
        return false;

    // The read is tracked before it is handed out, or the copy is not handed out: an untracked read could be
    // overwritten by the next capture while the bridge's list still reads it. What Track changed is kept so
    // AbandonCapturedGuides can undo it when the bridge drops its recording without executing it.
    // isReadInFlight decides on presentReadUsage, so the bridge's read goes there too, or every slot it read
    // would look in flight for good and the bridge would lose its guides after four frames.
    const auto savedUsage = clone.readUsage;
    const auto savedPresentUsage = clone.presentReadUsage;
    const bool savedTracked = clone.readTracked;
    clone.presentReadUsage = {};
    if (!DlssNr::Submission::Track(cmdList, clone.readUsage) ||
        !DlssNr::Submission::Track(cmdList, clone.presentReadUsage))
    {
        clone.readUsage = savedUsage;
        clone.presentReadUsage = savedPresentUsage;
        return false;
    }
    clone.readTracked = true;
    g_bridgeRead = { true, slot, cmdList, savedUsage, savedPresentUsage, savedTracked };

    *depth = clone.depth;
    *motion = clone.motion;
    *frame = clone.frame;
    // The owned copies rest in NON_PIXEL_SHADER_RESOURCE, whatever the game's barrier override says.
    frame->DepthState = (int) D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    frame->MotionState = (int) D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return true;
}
} // namespace Dx12Internal

bool CapturedPresentGuides(ID3D12GraphicsCommandList* cmdList, ID3D12Resource** depth, ID3D12Resource** motion,
                           DlssNrFrameInfo* frame, ID3D12CommandQueue* queue, const void* presenter,
                           unsigned int presentWidth, unsigned int presentHeight, bool* gameGuidesHold)
{
    if (gameGuidesHold != nullptr)
        *gameGuidesHold = false;

    NoteBridgePresentedExtent(presentWidth, presentHeight);

    if (depth == nullptr || motion == nullptr || frame == nullptr)
        return false;

    // The D3D11 host calls from its own present thread; the pointers stay alive past this because a
    // replaced capture is parked, not released.
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    // One recording, one read: the previous one has executed or been abandoned by now.
    g_bridgeRead = {};

    const auto isEligible = [queue](void* token) -> bool
    {
        if (token == nullptr)
            return false;
        auto& writeUsage = *static_cast<DlssNr::Submission::Usage*>(token);
        if (queue != nullptr && DlssNr::Submission::SubmittedOnlyOn(writeUsage, queue))
            return true;
        return DlssNr::Submission::Completed(writeUsage);
    };

    const size_t pickedSlot = g_presentGuideRing.PickForPresent(isEligible, NowMs());

    // The identity rules over that pick (present-real-guides.md, step 1): the bridge reads the slot they return, the
    // pick or none. A refused slot is not touched, and the host falls back to its zeros or synthesized motion.
    const GuideAsk guides = AskBridgeGuides(cmdList, presenter, presentWidth, presentHeight, pickedSlot);
    const bool read = ReadCapturedGuides(guides.decision.slot, cmdList, depth, motion, frame);

    const bool hold = CountBridgeGuides(guides, pickedSlot,
                                        DlssNr::PresentGuideIdentity::OutcomeOf(guides.decision, read), presenter);
    if (gameGuidesHold != nullptr)
        *gameGuidesHold = hold;

    return read;
}

// The bridge abandoned the recording its captured guides were read in: the read never executes, so the slot's
// read usage goes back to what it was before that Track, or the slot would count as being read for good.
void AbandonCapturedGuides()
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    if (!g_bridgeRead.valid)
        return;
    auto& clone = g_presentGuideClones[g_bridgeRead.slot];
    clone.readUsage = g_bridgeRead.usage;
    clone.presentReadUsage = g_bridgeRead.presentUsage;
    clone.readTracked = g_bridgeRead.tracked;
    if (g_bridgeRead.list != nullptr)
        DlssNr::Submission::Abandon(g_bridgeRead.list);
    g_bridgeRead = {};
}

} // namespace DlssNr
