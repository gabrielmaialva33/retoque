#include "pch.h"

// Lifetime/admission helpers moved whole from DlssNr_Dx12.cpp. Shared state remains defined there;
// lock ownership, function-local statics and release ordering are unchanged.
#include "DlssNr_Dx12_Internal.h"
#include <dlssnr/DlssNr_ReplayClock.h>
#include <dlssnr/DlssNr_Chain.h>
#include <State.h>
#include <dxgi1_4.h>
#include <menu/menu_overlay_base.h>
#include <framegen/IFGFeature_Dx12.h>
#include <shaders/output_scaling/OS_Dx12.h>
#include "DlssNr_Stabilizer_Dx12.h"
#include "DlssNr_UiMask_Dx12.h"
#include "DlssNr_Cadence_Dx12.h"
#include "DlssNr_MotionProbe_Dx12.h"
#include <algorithm>

namespace DlssNr::Dx12Internal
{
// StableExtent discriminates on two dimensions and one scalar, but the rebuild it guards keys on
// more than that: resolutionChanged compares the native width and height as well as the working
// ones. Rounding hides the difference -- at scale 0.25 a native 3440 and 3441 both land on 860 --
// so alternating them walks past an already-settled gate and rebuilds anyway. Folding the native
// extent and the format into the scalar makes the gate observe the whole contract the rebuild
// reads, rather than a projection of it.
uint32_t PostContract(unsigned int nativeWidth, unsigned int nativeHeight, uint32_t format)
{
    uint32_t h = 2166136261u;
    for (uint32_t v : { (uint32_t) nativeWidth, (uint32_t) nativeHeight, format })
    {
        h ^= v;
        h *= 16777619u;
    }
    return h;
}

void ChainStatus(const char* reason)
{
    if (g_chainStatus.exchange(reason) != reason)
        LOG_INFO("DLSS-NR chain: {}", reason);
}

// Published, and logged once on change, for a refusal or a running cadence. A forced model frame -- a reset,
// a change, a gap -- is one frame's event, not a state, and leaves the status alone. A second call in one
// present is published: a title that does it every present never carries, and should say why.
// feedback: adaptive cadence's newest sample (cadence-adaptive.md), logged with the change it explains, so a game run
// says which measurement moved the interval and how far it was from the thresholds. Null when adaptive is not active.
void PublishCadence(const DlssNr::Cadence::Decision& decision, const DlssNr::Cadence::Adaptive::Sample* feedback,
                    uint64_t evaluation, int64_t nowMs)
{
    using DlssNr::Cadence::Reason;
    if (DlssNr::Cadence::IsForced(decision.reason) && decision.reason != Reason::SamePresent)
        return;

    const char* status = DlssNr::Cadence::Describe(decision.reason, decision.cadence);
    if (g_cadenceStatus.exchange(status) == status)
        return;
    if (feedback == nullptr)
        LOG_INFO("DLSS-NR cadence: {}", status);
    else if (!feedback->valid)
        LOG_INFO("DLSS-NR cadence: {} (adaptive: no valid feedback)", status);
    else
        LOG_INFO("DLSS-NR cadence: {} (adaptive: motion p90 {:.2f} px/frame at 1080 lines, carry rejected {:.1f}%, "
                 "sample {} evaluations / {} ms old)",
                 status, feedback->motion, feedback->rejection * 100.0f, evaluation - feedback->present,
                 nowMs - feedback->ms);
}

// Whether any frame generation is turning this pass's frames into more than one present: OptiScaler's own
// (FSR-FG, XeFG, reprojection, synthesized) running and not paused, the game's DLSS-G through NGX (the
// multi-frame count its evaluate reports, cleared after six evaluates without one), or a DLSS-G mode set
// through Streamline. Model cadence stands down for all of them unless the user opts in: frame times then
// alternate long and short, and DLSS-G was measured unable to pace through that.
bool FrameGenerationActive()
{
    auto& state = State::Instance();
    if (auto* fg = state.currentFG; fg != nullptr && fg->IsActive() && !fg->IsPaused())
        return true;
    return state.dlssgDetectedInterpolationCount > 0 || state.dlssgLastSetMode != sl::DLSSGMode::eOff;
}

// docs/dlssnr/design/pause-without-frame-gen.md. Whether to skip the pass because the game turned its own DLSS-G off.
// The signal is what the game asked Streamline for, recorded before any OptiScaler policy (the menu interlock, the
// interpolation override, substituting our own DLSS-G), so none of those can pause it. Under g_nrMutex. Arms only
// after the game was seen asking for DLSS-G this session; never while our menu is open; and only once the off state
// has held for half a second. While paused, the next evaluation is marked to reset its history: the model did not
// see the frames in between, and any reset the game asked for then was not delivered.
bool PausedWithoutFrameGen(const Config& cfg)
{
    using Clock = std::chrono::steady_clock;
    static bool seenOn = false;
    static std::optional<Clock::time_point> offSince;
    static bool paused = false;
    const bool on = State::Instance().dlssgGameRequestsFg.load(std::memory_order_relaxed);
    if (on)
        seenOn = true;
    const bool candidate =
        cfg.DlssNrPauseWithoutFrameGen.value_or_default() && seenOn && !on && !MenuOverlayBase::IsVisible();
    if (!candidate)
        offSince.reset();
    else if (!offSince)
        offSince = DlssNr::ReplayClock::Now();
    const bool now = offSince && DlssNr::ReplayClock::Now() - *offSince >= std::chrono::milliseconds(500);
    if (now)
        g_resetOnReturn = true;
    if (now != paused)
    {
        paused = now;
        LOG_INFO("DLSS-NR {} (PauseWithoutFrameGen)",
                 now ? "paused: the game turned its frame generation off" : "resumed after the frame-generation pause");
    }
    return now;
}

void AdvanceNrClock() { DlssNr::Submission::Advance(++g_nrTicks); }

const char* AdmissionRefusal(DlssNr::Submission::Detail::Admission admission)
{
    using DlssNr::Submission::Detail::Admission;
    switch (admission)
    {
    case Admission::InFlight:
        return "More than three earlier NR recordings still on the GPU; new recording skipped this frame";
    case Admission::Queues:
        return "Earlier NR recordings still running on two GPU queues; new recording skipped until one finishes";
    case Admission::SameList:
        return "This command list already holds an unsubmitted NR recording; new recording skipped";
    case Admission::Pending:
        return "An NR recording is still waiting for the game to submit it; new recordings skipped until it runs or "
               "is discarded";
    case Admission::Behind:
        return "The GPU is eight NR recordings behind; new recording skipped this frame";
    case Admission::Serial:
        return "Waiting for the previous NR recording to finish on the GPU (one queue not yet established, or more "
               "than one in use)";
    case Admission::Replay:
    default:
        return "Waiting for previous NR recording Reset and all GPU fences; new recording skipped (throughput may "
               "decrease)";
    }
}

bool AdaptiveCadenceRequested()
{
    const auto& config = *Config::Instance();
    return config.DlssNrCadenceAdaptive.value_or_default() &&
           DlssNr::Cadence::Requested(config.DlssNrCadence.value_or_default()) > 1;
}

bool WantsTrackedRecording(const DlssNrPassSnapshot& requested)
{
    return g_tracked ||
           (!g_legacyRecorded && (requested.Count > 1 || requested.Individual || AdaptiveCadenceRequested()));
}

bool TrackNrRecording(ID3D12GraphicsCommandList* cmd, const DlssNrPassSnapshot& requested,
                      DlssNr::Chain::RecordingLease* lease)
{
    const bool optIn = requested.Count > 1 || requested.Individual || AdaptiveCadenceRequested();
    if (!g_tracked && optIn && !g_legacyRecorded)
        g_tracked = true;
    if (!g_tracked)
    {
        if (optIn)
            ChainStatus(
                "Restart required: existing NR work has no submission lifetime evidence; single/master retained");
        g_legacyRecorded = true;
        return true;
    }
    if (!lease || !lease->Valid(g_recordings, reinterpret_cast<uintptr_t>(cmd)))
    {
        ChainStatus("Another NR recording scope owns the resources; new recording refused");
        return false;
    }
    if (lease->Tracked())
        return true; // Explicit pre scope borrowing its own recording.
    // Bounded in flight, not serialized (docs/dlssnr/design/chain-in-flight.md): earlier recordings need only be
    // finished on the GPU, or few enough and on one queue. Waiting for the game's Reset of its own list skipped
    // frames whenever it rotated lists, and for good once it stopped using one.
    if (DlssNr::Submission::Strict())
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            LOG_WARN("DLSS-NR chain: the game executed a command list again without resetting it; tracked NR now "
                     "waits for Reset and all GPU fences for the rest of the session");
        }
    }
    const auto admission =
        g_haveTrackedUsage ? DlssNr::Submission::Admit(g_usage, cmd) : DlssNr::Submission::Detail::Admission::Admit;
    if (!lease->MayTrack(g_haveTrackedUsage, admission == DlssNr::Submission::Detail::Admission::Admit))
    {
        ChainStatus(AdmissionRefusal(admission));
        return false;
    }
    if (!RetiredCapacity())
    {
        ChainStatus("Retired resource limit reached: pending/unknown GPU recordings retained; rebuild refused until "
                    "completion or restart");
        return false;
    }
    if (!DlssNr::Submission::Track(cmd, g_usage, true))
    {
        ChainStatus(DlssNr::Submission::LastRefusal());
        return false;
    }
    g_haveTrackedUsage = true;
    lease->MarkTracked();
    return true;
}

bool VideoMemory(ID3D12Device* device, DXGI_QUERY_VIDEO_MEMORY_INFO& memory)
{
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter3* adapter = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    const HRESULT found = factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
    factory->Release();
    if (FAILED(found))
        return false;
    const HRESULT result = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory);
    adapter->Release();
    return SUCCEEDED(result);
}

void ParkNrFeature(void*& feature)
{
    if (feature == nullptr)
        return;

    NrRetired r;
    r.tracked = g_tracked;
    if (g_tracked)
        r.usage = g_usage;
    r.feature = feature;
    feature = nullptr;
    g_nrRetired.push_back(r);
}

void ParkNrResource(ID3D12Resource*& res)
{
    if (res == nullptr)
        return;

    NrRetired r;
    r.tracked = g_tracked;
    if (g_tracked)
        r.usage = g_usage;
    r.resource = res;
    res = nullptr;
    g_nrRetired.push_back(r);
}

// Parked like everything else, tracked or not. The scaler owns descriptor heaps and buffers that the
// previous frame's recording still references; deleting it here on the untracked path freed them
// under the GPU whenever the NR downscaler changed during supersampling.
void ParkNrScaler(OS_Dx12*& scaler)
{
    if (!scaler)
        return;

    NrRetired r;
    r.tracked = g_tracked;
    if (g_tracked)
        r.usage = g_usage;
    r.scaler = scaler;
    scaler = nullptr;
    g_nrRetired.push_back(r);
}

// The stabilizer owns its history, descriptor heaps and constants, which the last recordings still
// reference; a new output size or switching it off parks it rather than deleting it.
void ParkNrStabilizer(DlssNr_Stabilizer_Dx12*& stabilizer)
{
    if (!stabilizer)
        return;

    NrRetired r;
    r.tracked = g_tracked;
    if (g_tracked)
        r.usage = g_usage;
    r.stabilizer = stabilizer;
    stabilizer = nullptr;
    g_nrRetired.push_back(r);
}

// The UI mask owns its history, descriptor heaps and constants, which the last recordings still reference;
// a new working size or switching it off parks it rather than deleting it.
void ParkNrUiMask(DlssNr_UiMask_Dx12*& uiMask)
{
    if (!uiMask)
        return;

    NrRetired r;
    r.tracked = g_tracked;
    if (g_tracked)
        r.usage = g_usage;
    r.uiMask = uiMask;
    uiMask = nullptr;
    g_nrRetired.push_back(r);
}

// The cadence's surfaces, descriptor heaps and constants are still referenced by the last recordings; a new
// working size, the cadence switched off, or a route it cannot run on parks them rather than deleting them.
void ParkNrCadence(DlssNr_Cadence_Dx12*& cadence)
{
    if (!cadence)
        return;

    NrRetired r;
    r.tracked = g_tracked;
    if (g_tracked)
        r.usage = g_usage;
    r.cadence = cadence;
    cadence = nullptr;
    g_nrRetired.push_back(r);
}

// Transfer 2's bundle, parked like the rest. Its NGX release happens when it is collected.
void ParkNrEnlarger(NrEnlarger*& enlarger)
{
    if (!enlarger)
        return;

    NrRetired r;
    r.tracked = g_tracked;
    if (g_tracked)
        r.usage = g_usage;
    r.enlarger = enlarger;
    enlarger = nullptr;
    g_nrRetired.push_back(r);
}

// shutdown: Ready only, no replay window (chain-in-flight.md).
void TickNrRetired(bool shutdown)
{
    std::vector<NrEnlarger*> enlargers;

    for (size_t i = 0; i < g_nrRetired.size();)
    {
        auto& retired = g_nrRetired[i];
        bool keep;
        if (retired.tracked)
        {
            const uint64_t age = DlssNr::Submission::Clock() - retired.parkedAt;
            const bool finished =
                shutdown ? DlssNr::Submission::Ready(retired.usage) : DlssNr::Submission::Settled(retired.usage);
            keep = age < static_cast<uint64_t>(retired.framesLeft) || !finished;
            // Recomputed every tick: a late first submission makes the epoch answerable again.
            const bool stuck = keep && !shutdown && age >= DlssNr::Submission::Detail::AgeOutTicks &&
                               DlssNr::Submission::OnlyUnanswerableLeft(retired.usage);
            if (stuck && !retired.stuck)
            {
                static bool said = false;
                if (!said)
                {
                    said = true;
                    LOG_WARN("DLSS-NR chain: a parked object waits on a recording no GPU fence will report (never "
                             "submitted, or its signal failed); it is kept, not freed, and no longer counts toward "
                             "the parking limit");
                }
            }
            retired.stuck = stuck;
        }
        else
            keep = --retired.framesLeft > 0;
        if (keep)
        {
            ++i;
            continue;
        }

        if (retired.feature != nullptr && g_nr.release != nullptr)
            g_nr.release(retired.feature);

        if (retired.resource != nullptr)
            retired.resource->Release();

        delete retired.scaler;
        delete retired.stabilizer;
        delete retired.uiMask;
        delete retired.cadence;
        delete retired.detailStats;
        delete retired.pulse;
        delete retired.motionProbe;

        if (retired.enlarger != nullptr)
            enlargers.push_back(retired.enlarger);

        g_nrRetired[i] = std::move(g_nrRetired.back());
        g_nrRetired.pop_back();
    }

    // Transfer 2's private DLSS goes once the list is whole again: releasing an NGX feature can re-enter the
    // queue hooks, and this list must not be mid-change then (wilsjo2, DlssNr_Dx12_Enlarge.cpp, GPL-3.0).
    for (NrEnlarger* enlarger : enlargers)
        delete enlarger;
}

bool RetiredCapacity()
{
    if (!g_tracked)
        return true;
    TickNrRetired();
    // Stuck objects are retained without evidence ever to free them; the first 64 do not block rebuilds, so one
    // discarded list cannot stop the chain, but past that a pathological game is refused rather than leaking more.
    size_t stuck = 0;
    for (const auto& retired : g_nrRetired)
        stuck += retired.stuck;
    const size_t counted = g_nrRetired.size() - std::min<size_t>(stuck, 64);
    // A rebuild can retire at most 18 objects (16, and peripheral compression's unpacked pair), the six of
    // the other colour format's set with a resolution change (ParkAltSurfaces), and Transfer 2's bundle (one
    // entry for its feature and its five textures). Refuse BEFORE recording or allocation, not after
    // unsubmitted slider/DRS changes already consumed arbitrary memory.
    return DlssNr::Chain::RetirementAllowed(counted);
}

// The inject point decides which buffer is being measured -- the upscaler's linear output or the
// finished frame in swapchain format -- so a reading taken before a change describes a different
// picture to one taken after. Everything else that depends on the format is invalidated here.
void ForgetCalibration()
{
    g_nr.calibCount = 0;
    g_nr.calibSuggestion = 0.0f;
    g_nr.calibSteadiness = 0.0f;
    g_nr.calibUsable = false;
    g_nr.calibWhy = "measuring...";
}

void ParkNrDetailStats(DlssNr_DetailStats_Dx12*& detailStats)
{
    if (!detailStats)
        return;

    NrRetired r;
    r.tracked = g_tracked;
    if (g_tracked)
        r.usage = g_usage;
    r.detailStats = detailStats;
    detailStats = nullptr;
    g_nrRetired.push_back(r);
}

// The pulse metric's pass, parked like the rest: its last recordings still reference its buffers and heaps.
void ParkNrPulse(DlssNr_Pulse_Dx12*& pulse)
{
    if (!pulse)
        return;

    NrRetired r;
    r.tracked = g_tracked;
    if (g_tracked)
        r.usage = g_usage;
    r.pulse = pulse;
    pulse = nullptr;
    g_nrRetired.push_back(r);
}

// Zero-motion detection's probe, parked like the pulse metric's pass: its last recordings still reference its buffers.
void ParkNrMotionProbe(DlssNr_MotionProbe_Dx12*& probe)
{
    if (!probe)
        return;

    NrRetired r;
    r.tracked = g_tracked;
    if (g_tracked)
        r.usage = g_usage;
    r.motionProbe = probe;
    probe = nullptr;
    g_nrRetired.push_back(r);
}

// A new chain generation did not fit beside the one it replaces (docs/dlssnr/design/chain-in-flight.md, "Rebuild
// headroom"). Park the whole current generation -- features, pass features, scratch -- as a rebuild on a resolution
// change would, so its memory goes once the GPU is done with it, and say whether there is anything still to wait
// for. False: nothing is parked or on its way out, so waiting would never make room.
bool MakeRoomForGeneration()
{
    bool parked = false;
    const auto park = [&parked](void*& feature)
    {
        parked |= feature != nullptr;
        ParkNrFeature(feature);
    };
    park(g_nr.feature);
    for (void*& f : g_nr.passFeature)
        park(f);
    for (auto& reset : g_nr.passReset)
        reset = true;
    for (ID3D12Resource** r : { &g_nr.output, &g_nr.colorCopy, &g_nr.hdrCopy, &g_nr.colorSmall, &g_nr.outputNative,
                                &g_nr.passPing, &g_nr.depthMatched, &g_nr.motionMatched })
    {
        parked |= *r != nullptr;
        ParkNrResource(*r);
    }
    g_nr.reset = true;
    if (parked)
        return true;
    return std::any_of(g_nrRetired.begin(), g_nrRetired.end(), [](const NrRetired& r) { return !r.stuck; });
}

void ParkAltSurfaces()
{
    auto& alt = g_nr.alt;

    for (ID3D12Resource** r :
         { &alt.output, &alt.colorCopy, &alt.hdrCopy, &alt.colorSmall, &alt.outputNative, &alt.passPing })
        ParkNrResource(*r);

    alt = {};
}

// Whether the set kept aside is the one a frame in this format, at this extent, needs.
bool AltSurfacesFit(DXGI_FORMAT format, unsigned int width, unsigned int height, unsigned int workWidth,
                    unsigned int workHeight)
{
    const auto& alt = g_nr.alt;
    return alt.output != nullptr && alt.format == format && alt.width == width && alt.height == height &&
           alt.workWidth == workWidth && alt.workHeight == workHeight;
}

// The format-bound surfaces follow the frame's colour format. The model does not: feature 18 is created
// for an extent and a preset and is never told a format, so a format change alone keeps every feature and
// pulses a history reset. It used to park them all, and a game that alternates formats (FF7 Rebirth,
// R11G11B10 in play and RGBA16F on cutscene camera cuts, found by hhkbble) paid a feature creation --
// 700 ms there -- on every cut, which is also the rebuild storm that exhausts the driver's latches.
//
// The surfaces of the format being left are kept aside rather than parked, and swapped back in when that
// format returns at the same extent, so an alternating game allocates both sets once. A swap frees nothing;
// what the slot held before, if it does not fit, is parked like any retired surface.
//
// An RGBA8/BGRA8 swap (docs/dlssnr/design/format-swap-history.md) is the same picture with two bytes swapped, read
// through views of each surface's own format, so at the same extent and through the same transfer it keeps the
// model's history and the calibration as well. Every other change resets both, as it always did. transfer is the
// frame's; the one the surfaces last served is kept beside them.
void ReleaseSurfacesIfFormatChanged(DXGI_FORMAT needed, unsigned int width, unsigned int height, unsigned int workWidth,
                                    unsigned int workHeight, const DlssNr::FormatTwin::Transfer& transfer)
{
    const DlssNr::FormatTwin::Transfer servedTransfer = g_nr.surfaceTransfer;
    g_nr.surfaceTransfer = transfer;

    if (g_nr.output == nullptr || g_nr.output->GetDesc().Format == needed)
        return;

    const auto outputDesc = g_nr.output->GetDesc();
    const auto frameDesc = g_nr.colorCopy != nullptr ? g_nr.colorCopy->GetDesc() : outputDesc;
    const bool reuse = AltSurfacesFit(needed, width, height, workWidth, workHeight);

    DlssNr::FormatTwin::Contract served;
    served.format = (int) outputDesc.Format;
    served.width = (unsigned int) frameDesc.Width;
    served.height = frameDesc.Height;
    served.workWidth = (unsigned int) outputDesc.Width;
    served.workHeight = outputDesc.Height;
    served.transfer = servedTransfer;

    DlssNr::FormatTwin::Contract wanted;
    wanted.format = (int) needed;
    wanted.width = width;
    wanted.height = height;
    wanted.workWidth = workWidth;
    wanted.workHeight = workHeight;
    wanted.transfer = transfer;

    const bool keepHistory = DlssNr::FormatTwin::KeepsHistory(served, wanted);

    if (keepHistory)
    {
        // A game that swaps every frame would write this every frame.
        static unsigned long long twinSwaps = 0;
        ++twinSwaps;
        if (twinSwaps <= 3 || twinSwaps % 1000 == 0)
            LOG_INFO("DLSS-NR surfaces: format {} -> {}, channel order only ({}); the model, its history and the "
                     "calibration are kept (swap {})",
                     (int) outputDesc.Format, (int) needed,
                     reuse ? "the set kept for it is swapped back in" : "a set is built for it, the old one kept aside",
                     twinSwaps);
    }
    else
    {
        LOG_INFO("DLSS-NR surfaces: format {} -> {} ({}); the model is kept", (int) outputDesc.Format, (int) needed,
                 reuse ? "the set kept for it is swapped back in" : "a set is built for it, the old one kept aside");

        ForgetCalibration();
    }

    if (!reuse)
        ParkAltSurfaces();

    auto& alt = g_nr.alt;
    std::swap(g_nr.output, alt.output);
    std::swap(g_nr.colorCopy, alt.colorCopy);
    std::swap(g_nr.hdrCopy, alt.hdrCopy);
    std::swap(g_nr.colorSmall, alt.colorSmall);
    std::swap(g_nr.outputNative, alt.outputNative);
    std::swap(g_nr.passPing, alt.passPing);
    alt.format = outputDesc.Format;
    alt.width = (unsigned int) frameDesc.Width;
    alt.height = frameDesc.Height;
    alt.workWidth = (unsigned int) outputDesc.Width;
    alt.workHeight = outputDesc.Height;

    if (keepHistory)
        return;

    // A different picture in a different encoding: no history carries over, for the first pass or the rest.
    g_nr.reset = true;
    for (auto& reset : g_nr.passReset)
        reset = true;
}

void ParkNrClone(ID3D12Resource*& res, const DlssNr::Submission::Usage& usage, bool tracked)
{
    if (res == nullptr)
        return;

    NrRetired r;
    r.tracked = tracked;
    if (tracked)
        r.usage = usage;
    r.resource = res;
    res = nullptr;
    g_nrRetired.push_back(r);
}
} // namespace DlssNr::Dx12Internal
