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
#include <dlssnr/DlssNr_DriverGate.h>
#include <dlssnr/DlssNr_DriverInitGate.h>
#include <dlssnr/DlssNr_NgxInfo.h>
#include <dlssnr/DlssNr_PresentColour.h>
#include <dlssnr/DlssNr_DisplayWhite.h>
#include <dlssnr/DlssNr_SwapchainColour.h>
#include <dlssnr/DlssNr_Enlarge.h>
#include <dlssnr/DlssNr_PrivateSr.h>
#include <dlssnr/DlssNr_MultiView.h>
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

namespace DlssNr::Dx12Internal
{
// Coverage belongs to an upscaler boundary, not to an arbitrary Dispatch call. Keep fallback
// separate from Stage 0, and do not count the normal Stage 1 post-path stand-down as another skip.
std::mutex g_coverageMutex;
DlssNr::Detail::StageCoverage g_coverage[4];
DlssNr::Detail::CoverageTotals g_coverageLogged[4];

struct CoverageScope
{
    // After the upscaler and after ray reconstruction are different placements with different
    // costs, and both used to report stage=after. Anyone comparing the three from these lines
    // could not separate them.
    int stage; // -1 inactive, 0 after, 1 before, 2 after-fallback, 3 after-rr
    DlssNr::Detail::CoverageSample sample;

    explicit CoverageScope(int which) : stage(which)
    {
        if (stage >= 0)
            sample.present = State::Instance().frameCount;
    }
    ~CoverageScope()
    {
        if (stage < 0)
            return;
        std::lock_guard<std::mutex> lock(g_coverageMutex);
        auto& coverage = g_coverage[stage];
        if (!coverage.Record(sample, std::chrono::steady_clock::now()))
            return;
        const auto& t = coverage.totals;
        auto& previous = g_coverageLogged[stage];
        LOG_INFO("DLSS-NR coverage stage={} calls={} model_ok={} model_failed={} applied_recorded={} "
                 "skipped={} fallback={} applied_present_ids={} skipped_present_ids={} present_id={} "
                 "extent={}x{} model={}x{} window_calls={} window_model_ok={} window_applied={} "
                 "window_skipped={} window_fallback={} coverage={} last=\"{}\" "
                 "evidence=CPU-recorded-not-GPU-complete",
                 stage == 0   ? "after"
                 : stage == 1 ? "before"
                 : stage == 2 ? "after-fallback"
                              : "after-rr",
                 t.calls, t.modelOk, t.modelFailed, t.applied, t.skipped, t.fallback, t.appliedPresents,
                 t.skippedPresents, sample.present, sample.width, sample.height, sample.modelWidth, sample.modelHeight,
                 t.calls - previous.calls, t.modelOk - previous.modelOk, t.applied - previous.applied,
                 t.skipped - previous.skipped, t.fallback - previous.fallback,
                 t.applied == previous.applied ? "ZERO_APPLIED_IN_WINDOW" : "APPLIED_IN_WINDOW", sample.reason);
        previous = t;
    }
};

// Everything the model is reached through. The snippet refuses callers whose module path does not
// contain "nvngx.dll", so the calls are made from a small library named for exactly that reason and
// shipped beside OptiScaler; see nvngx.dll_dlssnr.dll.
// Model entry-point aliases are declared in DlssNr_Dx12_Internal.h.

// One per back buffer, so an allocator is never reset while its frame is still in flight.

// Internal type: NrState (DlssNr_Dx12_Internal.h)

NrDeviceOwner g_nrDeviceOwner;
NrState g_nr;
std::unique_ptr<DlssNr_Dx12> g_compose;

// The before-upscale path's own surface: the copy of the game's colour that carries the model's edit
// into the upscaler. Sized to the render subrect and reallocated when that moves, which in a
// dynamic-resolution game is often -- see the design note for what that costs and what to do about it.
// Internal type: PreUpscaleState (DlssNr_Dx12_Internal.h)

PreUpscaleState g_pre;
std::mutex g_preMutex;
DlssNr::Detail::StableExtent g_preExtent;
DlssNr::Detail::CreationFrameGate g_preCreation;
ID3D12Device* g_preDevice = nullptr; // Held by the scratch; refuse a different device this session.
bool g_preAllocationFailed = false;
bool g_lastBeforeUpscale = false;
bool g_lastAfterRayReconstruction = false;
std::atomic<const char*> g_preStatus { "Waiting for a supported upscale evaluation" };

// The timing contract, capture triggers, detail-measure and pulse helpers, the exposure meter and calibration
// readbacks and ReportSkipOnce live in DlssNr_Dx12_Diagnostics.cpp.

uint64_t g_timingEvaluations = 0;

// Writes matched before/after frames on request, so comparisons stop depending on video.
capture::FrameCapture g_capture;

unsigned long long g_frames = 0;

// A capture requested from outside the game: when the render path has no fence of its own, the write
// waits until this frame count, by which point the GPU is certainly past the copies.
unsigned long long g_captureWriteAtFrame = 0;

std::filesystem::path g_dllDir;

// Switching inject points changes the surface format underneath the scratch set: the finished frame
// works in the swapchain's format, the pre-frame-generation path in the upscaler's. A stale set either
// clamps linear HDR into an 8-bit texture -- wrong brightness until something forces a rebuild -- or
// hands CopyResource mismatched formats, which fails silently and makes the whole pass appear to do
// nothing. So the set is torn down whenever the format it was built for is not the format needed now.
// Tracking is sticky: lifetime evidence must begin before the first recorded NR use.
bool g_tracked = false;
bool g_legacyRecorded = false;
DlssNr::Submission::Usage g_usage;
bool g_haveTrackedUsage = false;
DlssNr::Chain::RecordingGate g_recordings;
DlssNr::Chain::Schedule g_chainSchedule;
DlssNr::Detail::StableExtent g_chainExtent;

// The same settling rule for the default path. It needs its own instance rather than sharing
// g_chainExtent: only one of the two runs in a session, but sharing would let a skip report the
// wrong gate, and a diagnostic that names the wrong cause is worse than none.
DlssNr::Detail::StableExtent g_postExtent;

std::atomic<unsigned int> g_activePasses { 0 };
std::atomic<const char*> g_chainStatus { "Single pass (master settings)" };

// Set by every entry point that finds the pass switched off, taken by the next Dispatch. The model's
// history is only ever told to reset by things the game or this module noticed, and the game cannot
// know NR was off: without this, switching it back on (the toggle key, mid-A/B) resumed on the
// history of whatever was on screen when it was switched off. Atomic because the entry points that
// see "off" do not take the lock Dispatch runs under.
std::atomic<bool> g_resetOnReturn { false };

// Whether the buffer the last Dispatch worked on was linear HDR (the colour transform on). For the menu,
// which offers highlight recovery only where it can act. False until something has run.
std::atomic<bool> g_lastLinearHdr { false };

// Model cadence (docs/dlssnr/design/model-cadence.md): which frames run the model and which carry its last edit.
// Decided once per Dispatch, under the pass's lock.
DlssNr::Cadence::Scheduler g_cadence;
DlssNr::Cadence::Adaptive::Controller g_cadenceAdaptive;
// The adaptive controller's clock: one tick per evaluation that reaches the cadence decision. Presents would not do:
// under frame generation several go by per evaluation, so feedback a few evaluations old would always read as stale.
uint64_t g_cadenceEvaluations = 0;

// What the menu shows under the control. Starts as "off", so a session that never turns the cadence on logs
// nothing about it.
std::atomic<const char*> g_cadenceStatus { DlssNr::Cadence::Describe(DlssNr::Cadence::Reason::Off, 1) };

bool RetiredCapacity();

// docs/dlssnr/design/chain-in-flight.md: the clock that ages epochs whose fences cannot answer, and parked objects.
// Every NR entry advances it before anything can return, so refused or skipped work still moves it. Under g_nrMutex.
uint64_t g_nrTicks = 0;

// Internal type: NrEnlarger (DlssNr_Dx12_Internal.h)

// Retired model features and surfaces are parked and freed a comfortable number of evaluates later.
// Releasing them immediately was the device hang: with frame generation the GPU runs several frames
// behind, this work rides the game's own queue that no module fence covers, and an NGX feature or
// scratch texture freed under in-flight work kills the device.
// Internal type: NrRetired (DlssNr_Dx12_Internal.h)

std::vector<NrRetired> g_nrRetired;

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to);

// Internal type: DetailMeasureSession (DlssNr_Dx12_Internal.h)

DetailMeasureSession g_detailMeasure;

// The pulse metric ([DlssNr] PulseMetric, docs/dlssnr/design/pulse-metric.md): one small reduction a frame while it
// is on, read back a few frames later and paired into light steps by the tracker. Everything but the published status
// is touched under g_nrMutex only; the menu reads the status under the session's own mutex.
// Internal type: PulseSession (DlssNr_Dx12_Internal.h)

PulseSession g_pulse;

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to);

// kMeterRowBytes and kMeterBytes are in DlssNr_Dx12_Internal.h: the meter readbacks in DlssNr_Dx12_Diagnostics.cpp
// use them too.

// Advanced by every NR scratch allocation, and read into every cached descriptor key.
//
// A cached descriptor is kept only while the key that describes it is unchanged, and the pointer and
// shape in that key cannot tell a re-created texture from the one it replaced when both land at the
// same address at the same size. Advancing here retires every key at once, which is coarse on purpose:
// a registry would have to be unwound along every release path, and scratch allocation happens on
// init, on resize and on feature rebuild -- never per frame -- so the cost of retiring everything is
// one pass of view creation at a moment that was already re-creating resources.
//
// Never reset on NR shutdown: the next session's allocator can hand back the addresses this one used.
// A wrapped generation is the one value that would make a stale key look fresh, so this saturates
// instead of wrapping -- and a saturated counter can no longer retire anything, so it turns descriptor
// reuse off rather than leaving keys that nothing can invalidate.
uint64_t g_nrScratchGeneration = 1;

// Reuse is only sound while the generation can still advance.
bool DescriptorReuseAvailable() { return g_nrScratchGeneration != UINT64_MAX; }

// Which object a cached descriptor names, as opposed to which address. The generation retires NR's own textures,
// but a game's resource has no generation: released and re-created at the same address with the same description
// -- a resize back to a size it had before can do exactly that -- it matches pointer and shape, and the descriptor
// left in the slot names freed memory. On native Windows that is a page fault and a removed device. So every
// resource a key names carries a token of ours as private data, set the first time it is bound: a new object at an
// old address has none, is given a new one, and no key made for the old object matches it. 0 is "no token", which
// DispatchPass never reuses.
// {6E1F3C2A-8B54-4D7E-9A61-2C5D0B7F4E93}
constexpr GUID kBindTokenGuid = { 0x6e1f3c2a, 0x8b54, 0x4d7e, { 0x9a, 0x61, 0x2c, 0x5d, 0x0b, 0x7f, 0x4e, 0x93 } };
// Atomic: the present route dispatches with g_nrMutex released.
std::atomic<uint64_t> g_nextBindToken { 1 };

uint64_t BindToken(ID3D12Resource* resource)
{
    uint64_t token = 0;
    UINT size = sizeof(token);
    if (SUCCEEDED(resource->GetPrivateData(kBindTokenGuid, &size, &token)) && size == sizeof(token) && token != 0)
        return token;

    token = g_nextBindToken.fetch_add(1, std::memory_order_relaxed);
    if (FAILED(resource->SetPrivateData(kBindTokenGuid, sizeof(token), &token)))
        return 0;

    return token;
}

// A change has to hold still before it is acted on: a slider being dragged reports a new value every
// frame, and each one would otherwise mean a new model.
constexpr unsigned long long kSettleFrames = 30;

// Guards the module's state. Every caller is now on the game's render thread, so this is no longer
// holding two threads apart -- but the D3D11-on-D3D12 bridge enters from its own call site, and the
// cost is a CPU-side lock on a path that already records command lists.
std::mutex g_nrMutex;

// Runs the pass inside the same state envelope every other OptiScaler compute pass runs in.
//
// The upscaler's own evaluate is wrapped like this by TryEvaluateOptiFeature: root-signature tracking
// off so the hooks do not record the pass's binds as the game's, heap capture skipped, and RestoreRoot
// afterwards to put the game's compute state back. Neural Rendering ran outside that envelope -- after
// the upscaler had already restored and re-armed -- so it left its own root signature and descriptor
// heaps bound and captured. On an ordinary engine the game rebinds and never notices. On a bindless
// engine (007 First Light, Monster Hunter Wilds, and the rest of the RestoreComputeSig* quirks) the
// game resumes off the pass's bindings and the device is removed.
//
// As RAII so every early return from the pass is covered. RestoreRoot is gated internally on the
// RestoreComputeSignature / RestoreGraphicSignature config, so this is a no-op on games that do not
// ask for it and only acts where it is needed.
struct ScopedNrStateEnvelope
{
    ID3D12GraphicsCommandList* cmd;
    ScopedSkipHeapCapture skipHeap;

    explicit ScopedNrStateEnvelope(ID3D12GraphicsCommandList* c) : cmd(c)
    {
        D3D12Hooks::SetRootSignatureTracking(false);
    }

    ~ScopedNrStateEnvelope()
    {
        D3D12Hooks::RestoreRoot(cmd);
        D3D12Hooks::SetRootSignatureTracking(true);
    }
};

// What peripheral compression did last, for the menu. Its own lock: the menu reads it from another thread and
// must not wait on a dispatch for it.
std::mutex g_peripheryStatusMutex;
DlssNr::PeripheryState g_peripheryStatus;

// Peripheral compression's layout for this frame (docs/dlssnr/design/peripheral-compression.md), or an inactive one
// with the reason. Builds the pass on first use; says on change what it decided, and drops the model's history when the
// layout moves, because the history then describes other pixels. A change of the packed extent is also a resolution
// change, which Dispatch rebuilds for on its own.
//
// Off -- the default -- is three config reads and a constant shape: no pass, no surface, no log line, and the
// working size Dispatch takes is the one it always took.
DlssNr::Periphery::Layout PeripheryLayoutFor(const Config& cfg, ID3D12Device* device, unsigned int width,
                                             unsigned int height, float workScale, unsigned int gridWidth,
                                             unsigned int gridHeight, bool afterUpscale, bool useProxy)
{
    namespace P = DlssNr::Periphery;

    P::Settings settings {};
    settings.enabled = cfg.DlssNrPeripheryCompression.value_or_default();
    settings.center = cfg.DlssNrPeripheryCenter.value_or_default();
    settings.work = cfg.DlssNrPeripheryWork.value_or_default();

    P::Layout layout = P::Build(settings, width, height, workScale);

    if (settings.enabled)
    {
        // The route's refusals first: they hold whatever the two numbers say.
        const char* refusal = !afterUpscale ? P::kReasonBeforeUpscale : useProxy ? P::kReasonUseProxy : nullptr;

        if (refusal == nullptr && layout.active)
        {
            if (!DlssNr_Periphery_Dx12::Available())
            {
                refusal = P::kReasonNoShader;
            }
            else
            {
                if (g_nr.periphery == nullptr && !g_nr.peripheryFailed)
                {
                    g_nr.periphery = new DlssNr_Periphery_Dx12("DLSS-NR periphery", device);

                    // Nothing was recorded with an object that failed to build, so it goes at once.
                    if (!g_nr.periphery->IsInit())
                    {
                        delete g_nr.periphery;
                        g_nr.periphery = nullptr;
                        g_nr.peripheryFailed = true;
                    }
                }

                if (g_nr.peripheryFailed || g_nr.periphery == nullptr)
                    refusal = P::kReasonPassFailed;
            }
        }

        if (refusal != nullptr)
        {
            layout.active = false;
            layout.reason = refusal;
        }
    }

    // The unpacked pair exists only while the feature runs, at the uniform grid's size.
    for (ID3D12Resource** unpacked : { &g_nr.unpackedProxy, &g_nr.unpackedAnswer })
    {
        if (*unpacked == nullptr)
            continue;

        const auto have = (*unpacked)->GetDesc();
        if (!layout.active || (unsigned int) have.Width != gridWidth || have.Height != gridHeight)
            ParkNrResource(*unpacked);
    }

    // Said, and the history dropped, once per change. Switched off it is one constant shape whatever the two
    // numbers say, so a default configuration never logs a line about a feature nobody turned on.
    struct Shape
    {
        bool enabled = false;
        bool active = false;
        const char* reason = "";
        uint32_t center = 0, work = 0;
        unsigned int modelW = 0, modelH = 0, gridW = 0, gridH = 0, frameW = 0, frameH = 0;
        bool operator==(const Shape&) const = default;
    };

    static Shape said {};
    Shape now {};

    if (settings.enabled)
        now = { true,          layout.active, layout.reason, settings.center, settings.work, layout.modelW,
                layout.modelH, gridWidth,     gridHeight,    width,           height };

    const float compression = layout.active ? std::min({ layout.x.compression[0], layout.x.compression[1],
                                                         layout.y.compression[0], layout.y.compression[1] })
                                            : 1.0f;

    if (!(now == said))
    {
        // The layout moving is what invalidates history here. A new packed extent alone is a resolution change,
        // which rebuilds the model and resets it anyway, and a title that alternates extents must not also
        // drop history on every alternation from this side.
        const bool layoutMoved = said.enabled != now.enabled || said.active != now.active ||
                                 said.center != now.center || said.work != now.work;
        if (layoutMoved && (said.active || now.active))
        {
            g_nr.reset = true;
            for (auto& reset : g_nr.passReset)
                reset = true;
        }

        if (now.active)
            LOG_INFO("DLSS-NR periphery: active, model {}x{} for a {}x{} frame (uniform grid {}x{}; centre {}, work "
                     "{}, compression {:.2f}; {:.0f} percent of the grid's pixels)",
                     now.modelW, now.modelH, width, height, gridWidth, gridHeight, settings.center, settings.work,
                     compression, 100.0 * layout.PixelShare(gridWidth, gridHeight));
        else if (now.enabled)
            LOG_INFO("DLSS-NR periphery: not active -- {}", now.reason);
        else
            LOG_INFO("DLSS-NR periphery: switched off");

        said = now;
    }

    // The menu shows the status only while the setting is on, and reads its age, so off writes nothing.
    if (settings.enabled)
    {
        std::lock_guard<std::mutex> lock(g_peripheryStatusMutex);
        g_peripheryStatus.seen = true;
        g_peripheryStatus.active = layout.active;
        g_peripheryStatus.reason = layout.reason;
        g_peripheryStatus.modelWidth = layout.modelW;
        g_peripheryStatus.modelHeight = layout.modelH;
        g_peripheryStatus.gridWidth = gridWidth;
        g_peripheryStatus.gridHeight = gridHeight;
        g_peripheryStatus.frameWidth = width;
        g_peripheryStatus.frameHeight = height;
        g_peripheryStatus.compression = compression;
        g_peripheryStatus.at = std::chrono::steady_clock::now();
    }

    return layout;
}

// ---------------------------------------------------------------------------------------------
// Transfer 2: the model's edit enlarged by a private DLSS Super Resolution. docs/dlssnr/design/dlss-enlargement.md.
//
// The rules are DlssNr_Enlarge.h's and the NGX calls DlssNr_PrivateSr's; this is the part that needs the
// pass's textures, its descriptor ring and its retirement list. wilsjo2's (OptiScaler-DLSSNR-PreSR-Multipass
// v0.8.91, DlssNr_Dx12_Enlarge.cpp, GPL-3.0), rewritten for this tree; what was changed is in the note.
// ---------------------------------------------------------------------------------------------

// The live bundle, whether it failed this session, and what the menu reads. Everything but the two atomics is
// touched only under g_nrMutex.
// Internal type: EnlargeSession (DlssNr_Dx12_Internal.h)

EnlargeSession g_enlarge;

} // namespace DlssNr::Dx12Internal

using namespace DlssNr::Dx12Internal;

// ---------------------------------------------------------------------------------------------
// The pass itself. Everything above is what it is made of; everything below is the shape the rest
// of OptiScaler sees.
// ---------------------------------------------------------------------------------------------

DlssNr_Dx12::DlssNr_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    // Five inputs, two outputs, one constant buffer, and a clamped linear sampler.
    //
    // The sampler exists because the model may be run below full resolution, in which case its answer
    // has to be read back at a different size from the frame it is being transferred onto.
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(InDevice, kSrvCount, kUavCount, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DlssNrConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (uint32_t i = 0; i < DLSSNR_NUM_OF_HEAPS; ++i)
    {
        auto result = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&_constantBuffers[i]));

        if (result != S_OK)
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
            return;
        }

        const D3D12_RANGE noRead { 0, 0 };
        result = _constantBuffers[i]->Map(0, &noRead, &_mappedConstants[i]);
        if (FAILED(result) || _mappedConstants[i] == nullptr)
        {
            LOG_ERROR("[{0}] Map constants error {1:x}", _name, (unsigned int) result);
            return;
        }
    }

    // Precompiled, with no source fallback. The shader used to be compiled at runtime from a string,
    // which would have meant no shader at all for anyone leaving UsePrecompiledShaders at its
    // default.
    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_cso, sizeof(DlssNr_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    if (!InitHeaps(InDevice, _frameHeaps, DLSSNR_NUM_OF_HEAPS))
        return;

    // Buffer addresses and sizes never change during this shader's lifetime.
    for (uint32_t i = 0; i < DLSSNR_NUM_OF_HEAPS; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv {};
        cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();
        cbv.SizeInBytes = sizeof(DlssNrConstants);
        InDevice->CreateConstantBufferView(&cbv, _frameHeaps[i].GetCbvCPU(0));
    }
    _init = true;
}

bool DlssNr_Dx12::DispatchPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                               ID3D12Resource* InSource, ID3D12Resource* InModel, ID3D12Resource* InOriginal,
                               ID3D12Resource* InMotion, ID3D12Resource* InPrevEdit, ID3D12Resource* OutTarget,
                               ID3D12Resource* OutKeep)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InSource == nullptr || OutTarget == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    // Every slot in the table gets a view, whether the mode reads it or not. An unbound descriptor is
    // not an empty read; it is a read from nothing, and the source stands in wherever a mode has
    // nothing of its own to put there.
    ID3D12Resource* const srvs[kSrvCount] = {
        InSource,
        InModel != nullptr ? InModel : InSource,
        InOriginal != nullptr ? InOriginal : InSource,
        InMotion != nullptr ? InMotion : InSource,
        InPrevEdit != nullptr ? InPrevEdit : InSource,
    };

    // Each view is created only when what it describes moved. BindKey carries both halves of that
    // question -- the resource's identity and the arguments the view is built from -- so a key that
    // compares equal means the descriptor already in the slot is byte for byte the one this call
    // would write.
    const bool reuse = DescriptorReuseAvailable();

    for (uint32_t i = 0; i < kSrvCount; ++i)
    {
        const auto desc = srvs[i]->GetDesc();
        const BindKey key { srvs[i],     BindToken(srvs[i]),    g_nrScratchGeneration, desc.Width,
                            desc.Height, desc.DepthOrArraySize, desc.MipLevels,        desc.Dimension,
                            desc.Format, kViewFormat,           kViewMipLevel,         kViewTranslateTypeless };

        if (reuse && key.token != 0 && _srvKey[slot][i] == key)
            continue;

        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i), kViewFormat, kViewTranslateTypeless);
        _srvKey[slot][i] = key;
    }

    ID3D12Resource* const uavs[kUavCount] = {
        OutTarget,
        OutKeep != nullptr ? OutKeep : OutTarget,
    };

    for (uint32_t i = 0; i < kUavCount; ++i)
    {
        const auto desc = uavs[i]->GetDesc();
        const BindKey key { uavs[i],     BindToken(uavs[i]),    g_nrScratchGeneration, desc.Width,
                            desc.Height, desc.DepthOrArraySize, desc.MipLevels,        desc.Dimension,
                            desc.Format, kViewFormat,           kViewMipLevel,         kViewTranslateTypeless };

        if (reuse && key.token != 0 && _uavKey[slot][i] == key)
            continue;

        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), kViewMipLevel, kViewTranslateTypeless);
        _uavKey[slot][i] = key;
    }

    std::memcpy(_mappedConstants[slot], &InConstants, sizeof(InConstants));

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    // Sized from the constants rather than from a resource, because the pass that shrinks the proxy
    // writes fewer pixels than its source has.
    const UINT dispatchWidth = (InConstants.Width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (InConstants.Height + _numThreadsY - 1) / _numThreadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

DlssNr_Dx12::~DlssNr_Dx12()
{
    for (uint32_t i = 0; i < DLSSNR_NUM_OF_HEAPS; ++i)
    {
        auto& buffer = _constantBuffers[i];
        if (buffer != nullptr)
        {
            if (_mappedConstants[i] != nullptr)
            {
                buffer->Unmap(0, nullptr);
                _mappedConstants[i] = nullptr;
            }
            buffer->Release();
            buffer = nullptr;
        }
    }
}

bool DlssNr_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* depth,
                           ID3D12Resource* motion, ID3D12Resource* output, const DlssNrFrameInfo& frame,
                           ID3D12CommandQueue* timingQueue, DlssNr::Detail::CoverageSample* coverage,
                           DlssNr::Chain::RecordingLease* recording)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    const Config& cfg = *Config::Instance();
    const auto reportSkip = [coverage](const char* why)
    {
        if (coverage != nullptr)
            coverage->reason = why;
        ReportSkipOnce(why);
    };

    AdvanceNrClock();

    // A fault the forwarder caught inside the model, on this frame or any before it, ends the session's
    // NR here: the forwarder already refuses to enter the model again, and this stops asking.
    NoteModelFault();

    // The pulse metric's sequence: every present the pass is asked to run at, counted before the first early return,
    // so one it skips breaks a pair of light steps instead of being bridged (docs/dlssnr/design/pulse-metric.md).
    NotePulsePresent(State::Instance().frameCount);

    // The comparison capture's ticket (docs/dlssnr/design/comparison-capture.md), before anything can return, so a
    // capture recorded on an earlier frame is handed to its writer -- or given up on -- whatever this frame does.
    // Idle, one relaxed load.
    DlssNr::PairCapture::Poll();

    if (g_nr.failed || cmdList == nullptr || colour == nullptr || depth == nullptr || motion == nullptr ||
        output == nullptr)
    {
        reportSkip(g_nr.failed ? "it already failed this session" : "a resource was missing");
        return false;
    }

    // Before any recording, state change or tracking (pause-without-frame-gen.md): the frame goes out as drawn.
    if (PausedWithoutFrameGen(cfg))
    {
        reportSkip("paused: the game turned its frame generation off");
        return false;
    }

    if (frame.GameGuides && (!frame.RenderSubrectWidth || !frame.RenderSubrectHeight))
    {
        reportSkip("the game's render geometry is unknown or incomplete");
        return false;
    }

    // Different configured arrival states cannot both describe the same resource. Reject before
    // the first transition; guessing would issue a barrier from a state that never existed.
    if (depth == motion || depth == colour || depth == output || motion == colour || motion == output)
    {
        reportSkip("aliased color/output/depth/motion resources cannot be normalized safely");
        return false;
    }

    // Before tracking, timing queries, barriers or writes: the pass and every supplied GPU
    // object must share the cache owner's device. The borrowed timingQueue hint is unused here.
    if (!DeviceChildBelongsTo(_device, cmdList) || !DeviceChildBelongsTo(_device, colour) ||
        !DeviceChildBelongsTo(_device, depth) || !DeviceChildBelongsTo(_device, motion) ||
        !DeviceChildBelongsTo(_device, output) ||
        (frame.ExposureTexture != nullptr &&
         !DeviceChildBelongsTo(_device, static_cast<ID3D12Resource*>(frame.ExposureTexture))) ||
        !g_nrDeviceOwner.Accept(_device))
    {
        reportSkip("NR device changed or inputs belong to another device; restart the process before enabling NR");
        return false;
    }

    g_activePasses = 0;
    auto passSnapshot = cfg.GetDlssNrPassSnapshot();
    if (frame.AfterRayReconstruction)
        passSnapshot.Count = DlssNr::PassConfig::BoundCount(cfg.DlssNrRRPasses.value_or_default());
    std::optional<DlssNr::Chain::RecordingLease> ownedRecording;
    if (!recording && WantsTrackedRecording(passSnapshot))
    {
        ownedRecording.emplace(g_recordings, reinterpret_cast<uintptr_t>(cmdList));
        recording = &*ownedRecording;
    }
    if (!TrackNrRecording(cmdList, passSnapshot, recording))
    {
        reportSkip(g_chainStatus.load());
        return false;
    }
    const bool useProxy = cfg.DlssNrUseProxy.value_or_default();
    const bool chainEnabled = g_tracked && !useProxy;
    const int dlssNrStage = cfg.DlssNrStage.value_or_default();
    const Scaler nrScalerForChain = cfg.DlssNrScalingDownscaler.value_or_default();
    const bool proxyProbe = cfg.DlssNrProxyProbe.value_or_default();
    // A present frame an HDR swapchain gave (docs/dlssnr/design/hdr-present-conversion.md section 4.2) has its white
    // from the host: the display's SDR white times the Paper white slider, from source 0 -- it has no exposure texture,
    // and an exposure or a scan anchor held from another route is not this frame's. Every other frame: the settings.
    const bool presentWhite = std::isfinite(frame.PresentWhite) && frame.PresentWhite > 0.0f;
    const float paperWhite = cfg.DlssNrWhitePointScale.value_or_default();
    const int whitePointSource = presentWhite ? 0 : cfg.DlssNrWhitePointSource.value_or_default();
    const float whitePointScale =
        presentWhite ? frame.PresentWhite * (std::isfinite(paperWhite) && paperWhite > 0.0f ? paperWhite : 1.0f)
                     : paperWhite;
    const float whitePointTrim = cfg.DlssNrWhitePointTrim.value_or_default();
    const bool scanInverted = cfg.DlssNrScanInverted.value_or_default();
    const float scanTrim = cfg.DlssNrScanTrim.value_or_default();
    const bool holdFrame = cfg.DlssNrHoldFrame.value_or_default();
    const uint32_t reversibleMode = cfg.DlssNrReversibleMode.value_or_default();

    // The two comparison controls, said out loud when they move.
    //
    // HoldFrame and ApplyModel exist to be toggled against each other on one frame: freeze the input,
    // turn the model's edit on and off, and the difference is the model's contribution. That makes
    // them the two settings most likely to be changing while someone is forming an opinion -- and
    // they were the only ones that left no trace, so a session spent comparing could not be
    // reconstructed afterwards from its own log. Every other input to the pass is already in the
    // composition contract; these two were not, because neither changes what is composed.
    //
    // On change only. A per-frame line would be sixty a second of something that moves by hand.
    {
        static int lastHold = -1, lastApply = -1;
        const int hold = holdFrame ? 1 : 0;
        const int apply = cfg.DlssNrApplyModel.value_or_default() ? 1 : 0;

        if (hold != lastHold || apply != lastApply)
        {
            LOG_INFO("DLSS-NR comparison controls: HoldFrame={}, ApplyModel={}", hold ? "on" : "off",
                     apply ? "on" : "off");
            lastHold = hold;
            lastApply = apply;
        }
    }
    const auto cfgPreset = cfg.DlssNrPreset.value_or_default();
    const float cfgIntensity = cfg.DlssNrIntensity.value_or_default();
    const float cfgLocalStructure = cfg.DlssNrLocalStructure.value_or_default();
    const float cfgLocalTone = cfg.DlssNrLocalTone.value_or_default();
    const float cfgSkinStructure = cfg.DlssNrSkinStructure.value_or_default();
    const auto cfgStyle = cfg.DlssNrStyle.value_or_default();
    const bool cfgAutoMask = cfg.DlssNrAutoMask.value_or_default();
    if (g_tracked && useProxy)
        ChainStatus("Driver proxy uses one pass and master settings; overrides are inactive");
    if (!chainEnabled)
    {
        // Ask for more than one pass only while the chain can still arm. TrackNrRecording arms it on
        // the first NR dispatch and, the moment a dispatch is recorded without the opt-in, sets
        // g_legacyRecorded and locks it off for the session -- that is the "Restart required" status.
        // The proxy path is one pass by construction.
        //
        // Promising a count that cannot run costs twice, and the second cost is the one that hid the
        // first. The extra-pass loop breaks on the first absent passFeature, so completed stays 1;
        // timing then rejects every sample as partial-chain. A session that silently lost multi-pass
        // also silently lost its measurement, and the only evidence was one log line at the moment of
        // the change. Say one, because one is what runs.
        const bool chainCanStillArm = !g_legacyRecorded && !useProxy;
        passSnapshot.Count = (frame.AfterRayReconstruction && chainCanStillArm)
                                 ? DlssNr::PassConfig::BoundCount(cfg.DlssNrRRPasses.value_or_default())
                                 : 1;
        passSnapshot.Individual = false;
        const auto master =
            DlssNrResolvedPassSettings { cfgIntensity, cfgLocalStructure, cfgLocalTone, cfgSkinStructure,
                                         cfgStyle,     cfgPreset,         cfgAutoMask };
        passSnapshot.Settings.fill(master);
    }
    const auto& firstSettings = passSnapshot.Settings[0];
    const auto presetForChain = chainEnabled ? firstSettings.Preset : cfgPreset;
    const float intensityForChain = chainEnabled ? firstSettings.Intensity : cfgIntensity;
    const float localStructureForChain = chainEnabled ? firstSettings.LocalStructure : cfgLocalStructure;
    const float localToneForChain = chainEnabled ? firstSettings.LocalTone : cfgLocalTone;
    const float skinStructureForChain = chainEnabled ? firstSettings.SkinStructure : cfgSkinStructure;
    const int styleForChain = static_cast<int>(chainEnabled ? firstSettings.Style : cfgStyle);
    const bool autoMaskForChain = chainEnabled ? firstSettings.AutoMask : cfgAutoMask;
    const auto stageForTiming = frame.AfterRayReconstruction ? "after-rr"
                                : !chainEnabled              ? "after"
                                                             : (dlssNrStage == 1 ? "after-fallback" : "after");
    const uint64_t observedFrame = State::Instance().frameCount;
    // Decisions read the replay clock: the steady clock outside an offline replay (offline-replay-seam.md).
    const auto now = DlssNr::ReplayClock::Now();

    const bool chainStable = !chainEnabled || g_chainSchedule.Stable(passSnapshot, now);
    if (chainEnabled && (!chainStable || !g_chainSchedule.Evaluate(observedFrame)))
    {
        reportSkip(!chainStable ? "chain settings are settling (500 ms)"
                                : "chain already evaluated at this observed present");
        return false;
    }

    ID3D12Resource* target = output;

    // What the model is shown. On the after-upscale path it is the target itself: the pass reads the
    // upscaler's output and writes the edit back over it, so the target is moved to a readable state
    // around every read. On the before-upscale path it is the game's own colour, which arrives in
    // NON_PIXEL_SHADER_RESOURCE -- the state NGX requires of every input -- is read there, and is
    // never written or transitioned by this pass at all.
    ID3D12Resource* const source = colour;
    const bool sourceIsTarget = source == target;
    const auto exposureResourceBarrier = static_cast<D3D12_RESOURCE_STATES>(
        cfg.ExposureResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    const auto depthResourceBarrier = static_cast<D3D12_RESOURCE_STATES>(
        cfg.DepthResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    const auto motionResourceBarrier = static_cast<D3D12_RESOURCE_STATES>(
        cfg.MVResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    const auto exposureBarrierState =
        sourceIsTarget ? exposureResourceBarrier : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto depthBarrierState = frame.DepthState >= 0 ? static_cast<D3D12_RESOURCE_STATES>(frame.DepthState)
                                   : sourceIsTarget      ? depthResourceBarrier
                                                         : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto motionBarrierState = frame.MotionState >= 0 ? static_cast<D3D12_RESOURCE_STATES>(frame.MotionState)
                                    : sourceIsTarget       ? motionResourceBarrier
                                                           : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Whether the resolve wrote the target this frame. See the header.
    bool wrote = false;

    // The state the upscaler left the output in. Every upscaler in this tree ends Evaluate by moving
    // the output to OutputResourceBarrier when the user set it (FFXFeature_Dx12.cpp:606 and the FSR2 /
    // XeSS equivalents), and leaves it in UNORDERED_ACCESS -- what its own compute wrote -- when they
    // did not. This pass then reads and writes the output as a UAV, so it normalises to that here and
    // restores the arrival state before every exit. When the config is unset the two states are equal
    // and Barrier() skips the no-op, so the default path is byte-identical.
    //
    // Unless the caller owns the output and says otherwise. The before-upscale path writes into a copy
    // of its own that never passed through an upscaler, and rests it in UNORDERED_ACCESS whatever the
    // config says about the game's output.
    const D3D12_RESOURCE_STATES outputArrival = frame.OutputState >= 0 ? (D3D12_RESOURCE_STATES) frame.OutputState
                                                : cfg.OutputResourceBarrier.has_value()
                                                    ? (D3D12_RESOURCE_STATES) cfg.OutputResourceBarrier.value()
                                                    : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Construct before every resource/state guard: the final timestamp is recorded only after
    // their destructors restore the caller's resources. The extra device query is measurement-only.
    const auto timingSettings = cfg.GetDlssNrGpuTimingSettings();
    const bool timingEnabled = timingSettings.Enabled && !useProxy;
    ID3D12Device* timingDevice = nullptr;
    if (timingEnabled)
        cmdList->GetDevice(IID_PPV_ARGS(&timingDevice));
    DlssNr::GpuTiming::Metadata initialTiming {};
    initialTiming.evaluationId = timingEnabled ? ++g_timingEvaluations : 0;
    initialTiming.presentId = observedFrame;
    DlssNr::GpuTiming::Evaluation timing(timingDevice, cmdList, initialTiming, timingEnabled, timingSettings.Interval);
    if (timingDevice)
        timingDevice->Release();

    struct RestoreOutput
    {
        ID3D12GraphicsCommandList* cmd;
        ID3D12Resource* target;
        D3D12_RESOURCE_STATES arrival;
        bool active;
        ~RestoreOutput()
        {
            if (active)
                Barrier(cmd, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, arrival);
        }
    } restoreOutput { cmdList, target, outputArrival, true };

    Barrier(cmdList, target, outputArrival, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The device behind the list being recorded, not the output's: NGX and D3D12Core dereference the list's, and a
    // wrapper layer leaves the two different (DlssNr_DeviceMatch.h). The prefix above held the list to _device.
    ID3D12Device* device = nullptr;

    if (FAILED(cmdList->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        reportSkip("the command list belongs to no D3D12 device");
        return false;
    }

    const HRESULT removedReason = device->GetDeviceRemovedReason();
    if (FAILED(removedReason))
    {
        reportSkip("D3D12 device removed or hung");
        LOG_ERROR("DLSS-NR: D3D12 device removed or hung (reason: 0x{:08X})", (unsigned) removedReason);
        DlssNr::Dred::Report(device);
        DlssNr::SessionGuardDeviceRemoved(); // evidence for the next launch, whatever the game does now
        device->Release();
        return false;
    }

    const bool restoreRequired =
        cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default();
    if (restoreRequired && !D3D12Hooks::CanRestoreRootSignature(cmdList))
    {
        reportSkip("command-list state could not be restored, including feature creation");
        device->Release();
        return false;
    }
    ScopedNrStateEnvelope stateEnvelope(cmdList);

    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    const auto width = (unsigned int) desc.Width;
    const auto height = desc.Height;

    // Depth and motion vectors are the upscaler's inputs and so are at render resolution, while colour
    // and output are at display resolution. The model takes that as a subrect per resource rather than
    // needing them resampled, which is why nothing here rescales anything.
    // The guides are the upscaler's inputs and so are at render resolution, while colour and output
    // are at display resolution. Their sizes come from the resources rather than from the caller:
    // one less thing a call site can get wrong, and the model takes the difference as a subrect per
    // resource rather than needing anything resampled.
    const D3D12_RESOURCE_DESC guideDesc = depth->GetDesc();
    const D3D12_RESOURCE_DESC motionDesc = motion->GetDesc();

    // Allocation bounds are independent of the valid picture. Only host-owned zero/synthetic guides
    // may use the host extent when no allocation is described; real game guides must stay bounded.
    unsigned int guideAllocWidth = (unsigned int) guideDesc.Width;
    unsigned int guideAllocHeight = guideDesc.Height;

    if ((guideAllocWidth == 0 || guideAllocHeight == 0) && !frame.GameGuides)
    {
        guideAllocWidth = width;
        guideAllocHeight = height;
    }

    // References describe the game's coordinate system, not the bounded resource region below.
    const unsigned int renderReferenceWidth = frame.RenderSubrectWidth ? frame.RenderSubrectWidth : guideAllocWidth;
    const unsigned int renderReferenceHeight = frame.RenderSubrectHeight ? frame.RenderSubrectHeight : guideAllocHeight;
    const bool haveDisplayReference = frame.DisplayWidth && frame.DisplayHeight;
    const unsigned int displayReferenceWidth = haveDisplayReference ? frame.DisplayWidth : width;
    const unsigned int displayReferenceHeight = haveDisplayReference ? frame.DisplayHeight : height;

    unsigned int guideWidth = guideAllocWidth;
    unsigned int guideHeight = guideAllocHeight;

    // What the game rendered wins over how big the texture is.
    //
    // The comment above says the sizes come from the resources so there is one less thing a call site
    // can get wrong, and that was right about call sites and wrong about the game. A dynamic
    // resolution title allocates its depth once at the maximum it will ever need and renders into
    // the corner; the resource then describes the allocation, not the picture, and the model gets
    // handed the stale margin as though it were scene.
    //
    // Bounded by the resource because a subrect larger than the texture is a game bug that would
    // otherwise become a read off the end of it.
    if (frame.RenderSubrectWidth != 0 && frame.RenderSubrectHeight != 0)
    {
        const unsigned int subW = std::min(frame.RenderSubrectWidth, guideWidth);
        const unsigned int subH = std::min(frame.RenderSubrectHeight, guideHeight);

        if (subW != guideWidth || subH != guideHeight)
        {
            static unsigned int saidW = 0, saidH = 0;

            if (saidW != subW || saidH != subH)
            {
                saidW = subW;
                saidH = subH;
                LOG_INFO("DLSS-NR guides: the game renders {}x{} into a {}x{} texture, so the model is "
                         "told the smaller number",
                         subW, subH, guideWidth, guideHeight);
            }
        }

        guideWidth = subW;
        guideHeight = subH;
    }

    // Where in the allocation the picture starts. A game that renders into a corner says so per
    // resource, and depth and motion vectors do not have to agree: bounded by the allocation for the
    // same reason the size is, and the size is then bounded by what is left after the offset.
    const unsigned int depthBaseX = std::min(frame.DepthSubrectBaseX, guideAllocWidth);
    const unsigned int depthBaseY = std::min(frame.DepthSubrectBaseY, guideAllocHeight);
    guideWidth = std::min(guideWidth, guideAllocWidth - depthBaseX);
    guideHeight = std::min(guideHeight, guideAllocHeight - depthBaseY);

    // Motion vectors are the one guide that is not always render resolution -- MVLowRes says which --
    // so the valid region is derived from the flag rather than shared with depth.
    const unsigned int wantedMotionWidth = DlssNr::GuideMatch::MotionReference(
        frame.MotionVectorsLowResolution, renderReferenceWidth, displayReferenceWidth);
    const unsigned int wantedMotionHeight = DlssNr::GuideMatch::MotionReference(
        frame.MotionVectorsLowResolution, renderReferenceHeight, displayReferenceHeight);

    unsigned int motionAllocWidth = (unsigned int) motionDesc.Width;
    unsigned int motionAllocHeight = motionDesc.Height;

    if ((motionAllocWidth == 0 || motionAllocHeight == 0) && !frame.GameGuides)
    {
        motionAllocWidth = wantedMotionWidth;
        motionAllocHeight = wantedMotionHeight;
    }

    const unsigned int motionBaseX = std::min(frame.MotionSubrectBaseX, motionAllocWidth);
    const unsigned int motionBaseY = std::min(frame.MotionSubrectBaseY, motionAllocHeight);
    const unsigned int motionWidth = std::min(wantedMotionWidth, motionAllocWidth - motionBaseX);
    const unsigned int motionHeight = std::min(wantedMotionHeight, motionAllocHeight - motionBaseY);

    // What the model is actually being shown, stated once per shape rather than only when something
    // looks wrong. The line above reports a surprise -- an allocation bigger than the frame in it --
    // and stays quiet otherwise; motion vectors now carry dimensions and an origin of their own, and
    // a wrong one is not a surprise anywhere, it is just a worse picture. The Vulkan path says the
    // same thing in the same shape, because these two are one behaviour read through two logs.
    {
        static unsigned int saidGuides[6] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
        const unsigned int now[6] = { depthBaseX, depthBaseY, motionBaseX, motionBaseY, motionWidth, motionHeight };

        if (!std::equal(std::begin(now), std::end(now), std::begin(saidGuides)))
        {
            std::copy(std::begin(now), std::end(now), std::begin(saidGuides));
            LOG_INFO("DLSS-NR guides: depth {}x{} at ({},{}), motion vectors {}x{} at ({},{}) in a {}x{} "
                     "texture, {}",
                     guideWidth, guideHeight, depthBaseX, depthBaseY, motionWidth, motionHeight, motionBaseX,
                     motionBaseY, motionAllocWidth, motionAllocHeight,
                     frame.MotionVectorsLowResolution ? "render resolution" : "display resolution");
        }
    }

    if (!guideWidth || !guideHeight || !motionWidth || !motionHeight)
    {
        reportSkip("a bounded depth or motion region is empty");
        return false;
    }

    g_nr.guideWidth = guideWidth;
    g_nr.guideHeight = guideHeight;
    g_nr.guideDepthInverted = frame.DepthInverted;

    // The game's own encoding, passed through. Every resource already carries a subrect saying how
    // big it is, so scaling by the resolution ratio on top of that counts it twice -- vectors come
    // out too long and the model warps its history past where the surface went.
    g_nr.guideMvScaleX = frame.MvScaleX;
    g_nr.guideMvScaleY = frame.MvScaleY;

    if (g_resetOnReturn.exchange(false))
        g_nr.reset = true;

    // Whether anything had already asked for a reset before the frame's own flag is read. The zero-guide
    // policy sets that flag on every frame for the model's sake alone; the stabilizer below must not read
    // it as a cut, so on those frames this is what it goes by.
    const bool resetBeforeFrameFlag = g_nr.reset;

    // The game's own vectors, read all zero in a moving scene by the motion probe (zero-motion-detection.md), are zero
    // guides by another name: ZeroGuideReset drops the model's history on them as it does on zero guides, as policy.
    // Off, nothing changes. A reset the game asked for this frame stays the game's.
    const bool deadMotionReset = DlssNr::MotionProbe::PolicyReset(
        MotionProbeDead(), frame.GameGuides, cfg.DlssNrZeroGuideReset.value_or_default(), frame.Reset);
    const bool frameReset = frame.Reset || deadMotionReset;
    const bool frameResetIsPolicy = deadMotionReset || frame.ResetIsPolicy;

    if (frameReset)
    {
        g_nr.reset = true;

        // Two counters, because only one of them is the game. ZeroGuideReset asks on every zero-guide
        // frame, and a log saying "the game asked" 69300 times described a game that asked for nothing.
        static unsigned long long resets = 0;
        static unsigned long long policyResets = 0;
        auto& count = frameResetIsPolicy ? policyResets : resets;
        ++count;

        if (count <= 3 || count % 100 == 0)
        {
            if (deadMotionReset)
                LOG_INFO("DLSS-NR: model history dropped on the game's all-zero motion vectors, as ZeroGuideReset "
                         "asks ({} so far)",
                         count);
            else if (frameResetIsPolicy)
                LOG_INFO("DLSS-NR: model history dropped on zero guides, as ZeroGuideReset asks ({} so far)", count);
            else
                LOG_INFO("DLSS-NR: the game asked for a history reset ({} so far)", count);
        }
    }

    // Logged whenever it changes, not once per session.
    //
    // A guide size change does not rebuild the feature -- the guides are handed over as subrects and
    // the output size is what the model is built for -- so a once-only line goes stale the moment the
    // player moves the quality slider, and every later line in the log is then read against numbers
    // that stopped being true. In Nioh 3 the session opened at DLAA, moved to 66% and ended at 33%,
    // and the log claimed 1920x1080 guides throughout.
    struct GuideReport
    {
        bool valid;
        bool depthInverted;
        float mvScaleX;
        float mvScaleY;
        unsigned int guideW;
        unsigned int guideH;
        unsigned int frameW;
        unsigned int frameH;
    };

    static GuideReport loggedGuides {};

    const GuideReport guidesNow {
        true,  g_nr.guideDepthInverted, g_nr.guideMvScaleX, g_nr.guideMvScaleY, guideWidth, guideHeight,
        width, (unsigned int) height
    };

    if (!loggedGuides.valid || loggedGuides.depthInverted != guidesNow.depthInverted ||
        loggedGuides.mvScaleX != guidesNow.mvScaleX || loggedGuides.mvScaleY != guidesNow.mvScaleY ||
        loggedGuides.guideW != guidesNow.guideW || loggedGuides.guideH != guidesNow.guideH ||
        loggedGuides.frameW != guidesNow.frameW || loggedGuides.frameH != guidesNow.frameH)
    {
        loggedGuides = guidesNow;
        LOG_INFO("DLSS-NR guides: depth {}, motion vector scale {} x {}, guides {}x{} for a {}x{} frame",
                 g_nr.guideDepthInverted ? "inverted" : "not inverted", g_nr.guideMvScaleX, g_nr.guideMvScaleY,
                 guideWidth, guideHeight, width, height);
    }

    if (proxyProbe)
        ProbeProxyDispatch(cmdList);

    if (!EnsureForwarder() || !EnsureCapabilityParams(device))
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return false;
    }

    // What the model works at. The frame and its edit stay full resolution; only the model's input and
    // answer change size, and the resolve enlarges (or minifies) the answer while compositing. Below 1
    // the model runs reduced and cheaper; above 1 it SUPERSAMPLES -- the proxy is upscaled to a larger
    // working size so the model denoises a super-native input, which the resolve then samples back down.
    // Capped at 2x: cost grows with the area and NGX acceptance above native is what this probe tests.
    // WorkingScale=auto depends on the GPU this device is on; see DlssNr_WorkingScale.h.
    DlssNr::NoteAdapter(device->GetAdapterLuid());
    const float configuredWorkScale =
        frame.AfterRayReconstruction ? cfg.DlssNrRRWorkingScale.value_or_default() : DlssNr::WorkingScale();
    // The upper bound is 1.0 for a caller with no real motion: above native the model refuses zero
    // guides (see DlssNrFrameInfo::AllowSupersampling), so clamping here turns a would-be refusal of
    // the user's chosen scale into the nearest size that runs, silently and every frame.
    const float maxWorkScale = frame.AllowSupersampling ? 2.0f : 1.0f;
    const float workScale = std::clamp(configuredWorkScale, 0.25f, maxWorkScale);

    // The uniform grid: the size the model works at, unless peripheral compression packs its input, in which
    // case it is the grid the model's answer is unpacked back onto (docs/dlssnr/design/peripheral-compression.md). From
    // here on workWidth/workHeight is the model's extent either way, so the feature, its output, the chain,
    // the settle gates and the format-flip set all follow the packed extent while the periphery runs.
    const auto gridWidth = (unsigned int) (width * workScale + 0.5f);
    const auto gridHeight = (unsigned int) (height * workScale + 0.5f);
    const DlssNr::Periphery::Layout periphery = PeripheryLayoutFor(cfg, device, width, (unsigned int) height, workScale,
                                                                   gridWidth, gridHeight, sourceIsTarget, useProxy);
    const bool peripheryActive = periphery.active;
    const auto workWidth = peripheryActive ? periphery.modelW : gridWidth;
    const auto workHeight = peripheryActive ? periphery.modelH : gridHeight;
    const bool reduced = workWidth != width || workHeight != height;

    // Say what the model is being asked for, when it changes.
    //
    // The working scale changes what the model receives more than any other control and left no trace
    // at all, so a session where someone moved the slider could not be read back. That cost a real
    // investigation: a run full of FAIL_InvalidParameter looked like a defect in the pass until the
    // person playing said they had raised the model to 200%, which is the one fact the log could not
    // supply. On change only -- it moves by hand.
    {
        static unsigned int lastW = 0, lastH = 0;

        if (workWidth != lastW || workHeight != lastH)
        {
            LOG_INFO("DLSS-NR working size: {}x{} at scale {:.2f} ({:.2f} Mpx), frame {}x{}{}", workWidth, workHeight,
                     workScale, (workWidth * (double) workHeight) / 1.0e6, width, height,
                     peripheryActive ? " -- packed by peripheral compression" : "");
            lastW = workWidth;
            lastH = workHeight;
        }
    }

    if (width < kDlssNrMinExtent || height < kDlssNrMinExtent || workWidth < kDlssNrMinExtent ||
        workHeight < kDlssNrMinExtent)
    {
        reportSkip("the frame or working size is below 64 pixels; the model is not built that small");
        device->Release();
        return false;
    }

    // The settling gates and the cadence signature watch the format with BGRA8 folded onto its RGBA8 twin: a game that
    // swaps the two (docs/dlssnr/design/format-swap-history.md) shows the pass one picture, and a gate that saw the
    // swap stood the model down for 500 ms each time -- forever, for a game that swaps every frame.
    const auto watchedFormat = static_cast<uint32_t>(DlssNr::FormatTwin::ChannelOrderCanonical((int) desc.Format));

    if (chainEnabled && !g_chainExtent.Observe(workWidth, workHeight, now, watchedFormat))
    {
        reportSkip("chain working resolution is settling (500 ms); no model reconstruction");
        device->Release();
        return false;
    }

    // The default path -- stage 0, one pass, master settings -- had no settling gate at all.
    // chainEnabled is g_tracked && !UseProxy, and g_tracked only turns on when multi-pass is opted
    // into, so the gate above never ran for the configuration almost everyone is using. A title that
    // alternates extents (an offscreen portrait rendered between main-scene frames is the reported
    // case) would rebuild the model on each alternation and retire the previous one every time.
    //
    // This is preventive hardening, not a fix for an observed failure: three deliberate resolution
    // changes in a real session went through the ungated path with no model failure and no growth
    // attributable to NR. The cost is real and is paid here -- after any extent change the model
    // stands down for 500 ms and the frame keeps the game's own colour, which under dynamic
    // resolution means pausing often.
    // sourceIsTarget is the post-upscale signal: after the upscaler the pass reads and writes the
    // same resource. Keying on it rather than on !chainEnabled matters twice. Stage 1 with a single
    // pass also leaves chainEnabled false, so the earlier condition gated the pre-upscale path too
    // and stacked a second 500 ms wait behind g_preExtent's on a cold start. And keying on the
    // configured stage instead would miss the case that matters most here: when the pre-upscale
    // scope declines -- ray reconstruction is the common one -- the after-upscale path serves that
    // evaluate and needs the gate just as much. The driver proxy is a post-upscale path too and is
    // deliberately included; it faces the same rebuild churn.
    // frame.ExtentIsStable last, so Observe is still called in exactly the cases it used to be and
    // the tracker's state stays coherent for callers that do alternate. Only the standing down is
    // skipped, and only for a caller that has said its extent cannot alternate.
    if (sourceIsTarget && !chainEnabled &&
        !g_postExtent.Observe(workWidth, workHeight, now, PostContract(width, (unsigned int) height, watchedFormat)) &&
        !frame.ExtentIsStable)
    {
        reportSkip("working resolution is settling (500 ms); no model reconstruction");
        device->Release();
        return false;
    }

    if (chainEnabled && (!g_nr.feature || g_nr.width != width || g_nr.height != height || g_nr.workWidth != workWidth ||
                         g_nr.workHeight != workHeight || g_nr.passBuilt[0] != firstSettings ||
                         (g_nr.output && g_nr.output->GetDesc().Format != desc.Format &&
                          !AltSurfacesFit(desc.Format, width, height, workWidth, workHeight))))
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO budget {};
        // Peripheral compression adds the unpacked pair, two RGBA16F surfaces on the uniform grid.
        const uint64_t surfaceBytes = uint64_t(workWidth) * workHeight * 48 + uint64_t(width) * height * 48 +
                                      (peripheryActive ? uint64_t(gridWidth) * gridHeight * 16 : 0);
        if (!VideoMemory(device, budget) ||
            !DlssNr::Chain::Admit(budget.Budget, budget.CurrentUsage, g_nr.measuredModelBytes, surfaceBytes))
        {
            // The usage counted here still includes the generation being replaced. Free that first and try again
            // once it is gone; only a refusal with nothing left to free is final for the session.
            if (MakeRoomForGeneration())
            {
                ChainStatus("Chain generation waits for the previous one to be freed (adapter headroom); NR resumes "
                            "once it is");
                reportSkip("chain generation waits for the previous one to be freed (adapter headroom)");
                device->Release();
                return false;
            }
            g_nr.failed = true;
            g_nr.reason = "chain generation refused: insufficient or unavailable adapter headroom (retry latched)";
            ChainStatus(g_nr.reason);
            reportSkip(g_nr.reason);
            device->Release();
            return false;
        }
    }

    // What the pass does to the colour beyond reading it: an RGBA8/BGRA8 swap keeps the model's history only through
    // the same one (DlssNr_FormatTwin.h).
    const DlssNr::FormatTwin::Transfer frameTransfer { frame.ColourIsLinearHdr, frame.PresentWhite,
                                                       frame.PresentQuantRel };
    ReleaseSurfacesIfFormatChanged(desc.Format, width, height, workWidth, workHeight, frameTransfer);

    const bool beforeUpscale = !sourceIsTarget;
    if (g_lastBeforeUpscale != beforeUpscale || g_lastAfterRayReconstruction != frame.AfterRayReconstruction)
    {
        g_nr.reset = true;
        g_lastBeforeUpscale = beforeUpscale;
        g_lastAfterRayReconstruction = frame.AfterRayReconstruction;
    }

    // The output stabilizer switched off, or on the stage where it cannot run: its history -- ~200 MB at
    // 4K -- goes back once the GPU is done with it, rather than waiting for a frame it will not see.
    // Written as !(x > 0) so a NaN from a hand-edited ini counts as off.
    if (!(cfg.DlssNrStabilizerStrength.value_or_default() > 0.0f) || beforeUpscale)
        ParkNrStabilizer(g_nr.stabilizer);

    // UI protection switched off, or a frame that is not a present source (no interface in it): the mask's
    // history goes back once the GPU is done with it.
    if (!cfg.DlssNrUiProtection.value_or_default() || !frame.PresentSource)
        ParkNrUiMask(g_nr.uiMask);

    const bool resolutionChanged =
        g_nr.width != width || g_nr.height != height || g_nr.workWidth != workWidth || g_nr.workHeight != workHeight;

    // The set kept for the other colour format was built for the old extent.
    if (resolutionChanged)
        ParkAltSurfaces();

    const bool placementChanged = g_nr.afterRayReconstruction != frame.AfterRayReconstruction;
    const bool presetChanged = g_nr.builtPreset != presetForChain;

    // Structural rebuild is only needed for resolution changes, pipeline placement, or weight preset changes.
    // Dynamic tuning (Style, Intensity, Tone, Structure, Skin, AutoMask) is passed directly to the forward
    // pass in evaluate without destroying the feature, matching NVIDIA's runtime architecture.
    const bool structuralRebuildRequired = resolutionChanged || placementChanged || presetChanged ||
                                           (chainEnabled && g_nr.passBuilt[0].Preset != firstSettings.Preset);

    const bool tuningChanged = (chainEnabled ? g_nr.passBuilt[0] != firstSettings : !TuningMatchesFeature(cfg));

    if (tuningChanged && !structuralRebuildRequired)
    {
        // When Model/Style or tuning sliders change, pulse a 1-frame temporal history reset
        // without destroying the feature, matching NVIDIA's CG2R_ResetTemporalHistoryOnControlChange contract.
        g_nr.reset = true;
        RecordBuiltTuning(cfg);
        if (chainEnabled)
            g_nr.passBuilt[0] = firstSettings;
    }

    if (g_nr.feature != nullptr && structuralRebuildRequired)
    {
        // Parked rather than released: with frame generation the GPU can still be several frames
        // deep in work that references all of it.
        ParkNrFeature(g_nr.feature);

        if (resolutionChanged)
            for (void*& f : g_nr.passFeature)
                ParkNrFeature(f);
        for (auto& reset : g_nr.passReset)
            reset = true;

        // Only a resolution change invalidates the scratch textures. Tuning does not, and throwing
        // them away for it would mean a reallocation every time a slider moves.
        if (resolutionChanged)
        {
            ParkNrResource(g_nr.output);
            ParkNrResource(g_nr.colorCopy);
            ParkNrResource(g_nr.hdrCopy);
            ParkNrResource(g_nr.colorSmall);
            ParkNrResource(g_nr.outputNative);
            ParkNrResource(g_nr.passPing);
            ParkNrResource(g_nr.depthMatched);
            ParkNrResource(g_nr.motionMatched);
        }
    }

    if (chainEnabled)
    {
        for (unsigned i = 1; i < 3; ++i)
        {
            if (g_nr.passFeature[i] &&
                (i >= passSnapshot.Count || g_nr.passBuilt[i].Preset != passSnapshot.Settings[i].Preset))
            {
                ParkNrFeature(g_nr.passFeature[i]);
                for (unsigned downstream = i; downstream < 3; ++downstream)
                    g_nr.passReset[downstream] = true;
            }
            else if (g_nr.passFeature[i] && g_nr.passBuilt[i] != passSnapshot.Settings[i])
            {
                g_nr.passReset[i] = true;
                g_nr.passBuilt[i] = passSnapshot.Settings[i];
            }
            if (resolutionChanged || g_nr.passBuilt[i] != passSnapshot.Settings[i])
                g_nr.passFailed[i] = false;
        }
    }

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, desc.Format, workWidth, workHeight);
        g_nr.colorCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.hdrCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.workWidth = workWidth;
        g_nr.workHeight = workHeight;
    }

    if (reduced && g_nr.colorSmall == nullptr)
        g_nr.colorSmall = CreateScratch(device, desc.Format, workWidth, workHeight);

    if (chainEnabled && (g_nr.output == nullptr || g_nr.colorCopy == nullptr || g_nr.hdrCopy == nullptr ||
                         (reduced && g_nr.colorSmall == nullptr)))
    {
        g_nr.failed = true;
        g_nr.reason = "chain scratch initialization failed; retry latched";
        reportSkip(g_nr.reason);
        device->Release();
        return false;
    }

    // The down-leg target is native (the answer is brought back to frame size before the resolve).
    if (workScale > 1.0f && g_nr.outputNative == nullptr)
        g_nr.outputNative = CreateScratch(device, desc.Format, width, height);

    if (g_nr.meter == nullptr)
    {
        g_nr.meter = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, kDlssNrMeterGrid, kDlssNrMeterGrid);

        D3D12_HEAP_PROPERTIES readback {};
        readback.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufferDesc {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = kMeterBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (auto& rb : g_nr.meterReadback)
        {
            if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
            {
                rb = nullptr;
                LOG_WARN("DLSS-NR: the white point meter could not allocate its readback; falling back "
                         "to the paper white slider");
            }
        }

        if (g_nr.meter != nullptr)
            LOG_INFO("DLSS-NR: white point meter up, {}x{} tiles", kDlssNrMeterGrid, kDlssNrMeterGrid);
    }

    if (g_nr.feature == nullptr && g_nr.output != nullptr && g_nr.colorCopy != nullptr && g_nr.hdrCopy != nullptr)
    {
        // Below NR's minimum driver, or after NGX answered FAIL_OutOfDate, no loader creates the model: the forwarder
        // and ModelLoader=direct call it themselves, and forcing it past that answer hung the GPU (DLSS Unlocked #60).
        if (DlssNr::DriverGate::Refuses())
        {
            g_nr.failed = true;
            g_nr.reason = DlssNr::DriverGate::Reason();
            LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
            device->Release();
            return false;
        }

        auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
        {
            g_nr.failed = true;
            g_nr.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
            LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
            device->Release();
            return false;
        }

        // Which driver, loader and model this is -- the first thing a report needs and, before this,
        // the one thing no log said. See docs/dlssnr/design/ngx-driver-616.md.
        DlssNr::NgxInfo::Describe(*snippet);

        // Before the build, so the model's own account of it ("Created feature ...") is in the log.
        DlssNr::ModelLog::Install(*snippet);

        SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
        DXGI_QUERY_VIDEO_MEMORY_INFO memoryBefore {};
        if (chainEnabled)
        {
            g_nr.creation[0] = {};
            if (!g_chainSchedule.Create(observedFrame, DlssNr::ReplayClock::Now()) ||
                !DlssNr::Submission::Track(cmdList, g_nr.creation[0]))
            {
                device->Release();
                return false;
            }
            VideoMemory(device, memoryBefore);
        }
        {
            // Never at the same moment as a vkCreateDevice (DlssNr_DriverInitGate.h): g_nrMutex -> the gate.
            DlssNr::DriverInitGate::Scope gate(DlssNr::DriverInitGate::Hold::Ngx, true,
                                               "the model's feature creation (D3D12)");
            g_nr.feature =
                g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), device,
                            cmdList, g_nr.capabilityParams, workWidth, workHeight, static_cast<int>(presetForChain),
                            intensityForChain, styleForChain, localStructureForChain, localToneForChain,
                            skinStructureForChain, autoMaskForChain ? 1 : 0,
                            // UI correction at the model's own default: with no UI layer fed to it there
                            // is nothing for it to correct.
                            1);
        }
        ++g_nr.featureGeneration; // a carried edit belongs to the feature that made it

        if (chainEnabled)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO memoryAfter {};
            if (VideoMemory(device, memoryAfter) && memoryAfter.CurrentUsage > memoryBefore.CurrentUsage)
                g_nr.measuredModelBytes = memoryAfter.CurrentUsage - memoryBefore.CurrentUsage;
            g_nr.creationFrame[0] = observedFrame;
            g_nr.passBuilt[0] = firstSettings;
        }

        if (g_nr.feature == nullptr)
        {
            g_nr.failed = true;
            g_nr.reason = "the model would not initialise";
            // The direct loader can refuse before init, when the model's imports are not the ones it adapts.
            if (g_nr.directRuntime && DlssNr::DirectRuntime::LoadError() != nullptr)
                g_nr.reason = DlssNr::DirectRuntime::LoadError();
            const auto initResult = (unsigned int) (g_nr.lastInit != nullptr ? *g_nr.lastInit : 0);
            const auto createResult = (unsigned int) (g_nr.lastCreate != nullptr ? *g_nr.lastCreate : 0);

            // FAIL_OutOfDate from the model's own init or create: the driver cannot run it. Latched, so neither a retry
            // nor a later pass, proxy or loader switch creates it in this process.
            const bool initOutOfDate = DlssNr::DriverGate::NoteNgxResult(initResult);
            const bool createOutOfDate = DlssNr::DriverGate::NoteNgxResult(createResult);
            if (initOutOfDate || createOutOfDate)
                g_nr.reason = DlssNr::DriverGate::kOutOfDateReason;

            // Cast before formatting. These are ints, and "0x{:X}" on a negative int prints
            // 0x-452FFFFF, which no one can decode back to 0xBAD00001.
            LOG_ERROR("DLSS-NR create failed: init 0x{:X} ({}), create 0x{:X} ({})", initResult,
                      NgxResultName(initResult), createResult, NgxResultName(createResult));
            NoteModelFault(); // a fault in init or create says where, here, while the log is on it
            device->Release();
            return false;
        }

        g_nr.width = width;
        g_nr.height = height;
        g_nr.afterRayReconstruction = frame.AfterRayReconstruction;
        g_nr.reset = true;
        RecordBuiltTuning(cfg);
        LOG_INFO("DLSS-NR running {}: target {}x{}, guides {}x{} (preset {}, intensity {}, style {})",
                 frame.AfterRayReconstruction ? "after Ray Reconstruction"
                                              : (sourceIsTarget ? "after upscale" : "before upscale"),
                 width, height, guideWidth, guideHeight, g_nr.builtPreset, g_nr.builtIntensity, g_nr.builtStyle);
        if (!sourceIsTarget)
        {
            g_preCreation.Created(State::Instance().frameCount);
            LOG_INFO("DLSS-NR created before the upscaler; waiting for an observed present before evaluation");
        }

        // Creating and evaluating a feature in the same command list is the dice-roll that hung the
        // GPU (every crash died on a creation frame). The creation goes through the game's own submit
        // first; the first evaluate happens next frame. One frame without the model is invisible.
        DlssNr::Replay::NoteSkip("the model was created on this frame; it first runs on the next");
        device->Release();
        return false;
    }

    if (g_nr.feature == nullptr)
    {
        device->Release();
        return false;
    }

    if (!sourceIsTarget && !g_preCreation.Ready(State::Instance().frameCount))
    {
        reportSkip("before the upscaler: feature creation has not crossed a present yet");
        device->Release();
        return false;
    }

    if (chainEnabled && (observedFrame <= g_nr.creationFrame[0] || !DlssNr::Submission::Completed(g_nr.creation[0])))
    {
        reportSkip("chain feature creation awaits a later present and actual submitted GPU completion");
        device->Release();
        return false;
    }

    // Build at most one extra instance per stable observed frame. Creation is not evaluation.
    if (chainEnabled && passSnapshot.Count > 1)
    {
        for (unsigned i = 1; i < passSnapshot.Count; ++i)
        {
            if (g_nr.passFailed[i])
                break;
            if (g_nr.passFeature[i])
                continue;
            DXGI_QUERY_VIDEO_MEMORY_INFO budget {};
            const uint64_t bytes = uint64_t(workWidth) * workHeight * 16;
            if (!VideoMemory(device, budget) ||
                !DlssNr::Chain::Admit(budget.Budget, budget.CurrentUsage, g_nr.measuredModelBytes, bytes))
            {
                g_nr.passFailed[i] = true;
                g_nr.passBuilt[i] = passSnapshot.Settings[i];
                ChainStatus(
                    "Additional pass refused: adapter budget/headroom unavailable or insufficient (retry latched)");
                break;
            }
            if (!g_chainSchedule.Create(observedFrame, DlssNr::ReplayClock::Now()))
                break;
            if (!g_nr.passPing)
                g_nr.passPing = CreateScratch(device, desc.Format, workWidth, workHeight);
            auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");
            if (!snippet)
                snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");
            g_nr.creation[i] = {};
            if (!g_nr.passPing || !snippet || !DlssNr::Submission::Track(cmdList, g_nr.creation[i]))
            {
                g_nr.passFailed[i] = true;
                g_nr.passBuilt[i] = passSnapshot.Settings[i];
                ChainStatus("Additional pass initialization failed (retry latched); last successful answer retained");
                break;
            }
            const auto& settings = passSnapshot.Settings[i];
            SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
            {
                DlssNr::DriverInitGate::Scope gate(DlssNr::DriverInitGate::Hold::Ngx, true,
                                                   "a chained pass's feature creation (D3D12)");
                g_nr.passFeature[i] =
                    g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), device,
                                cmdList, g_nr.capabilityParams, workWidth, workHeight, (int) settings.Preset,
                                settings.Intensity, (int) settings.Style, settings.LocalStructure, settings.LocalTone,
                                settings.SkinStructure, settings.AutoMask ? 1 : 0, 1);
            }
            ++g_nr.featureGeneration;
            g_nr.passBuilt[i] = settings;
            g_nr.creationFrame[i] = observedFrame;
            g_nr.passReset[i] = true;
            g_nr.passFailed[i] = !g_nr.passFeature[i];
            LOG_INFO("DLSS-NR chain: create pass {} result {}; budget={} usage={} measured-model={}; awaiting "
                     "submitted completion",
                     i + 1, g_nr.passFeature[i] ? "created" : "failed", budget.Budget, budget.CurrentUsage,
                     g_nr.measuredModelBytes);
            break;
        }
    }

    // The upscaler has just written this, so it is a UAV. The model needs it readable.
    // Whether the buffer the upscaler just wrote is linear HDR or an already tone-mapped picture is not
    // something to assume: the game says so, in the flags it created its own DLSS feature with. Running
    // the colour transform over a frame that has already been through a tonemapper is pure damage, and
    // skipping it on one that has not leaves the model reading ordinary values as enormously bright.
    // Both have to agree: the caller says what the game intends, the format says what the surface can
    // actually hold. A game that claims HDR while rendering into eight bits gets its frame encoded
    // twice otherwise.
    const bool gameSaysHdr = frame.ColourIsLinearHdr;
    const bool isHdrBuffer = gameSaysHdr && FormatCanHoldLinearHdr(desc.Format);
    g_lastLinearHdr.store(isHdrBuffer);

    static bool reportedHdr = false;

    if (!reportedHdr)
    {
        reportedHdr = true;
        LOG_INFO("DLSS-NR: the game's DLSS buffer is {} so the colour transform is {}",
                 isHdrBuffer ? "linear HDR" : "already tone-mapped", isHdrBuffer ? "on" : "off");
    }

    const bool haveCodec = IsInit();

    if (!haveCodec)
    {
        g_nr.failed = true;
        g_nr.reason = "the colour codec would not compile";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return false;
    }

    // What the upscaler produces is linear HDR with an open-ended range; the model was trained on
    // finished, sRGB-encoded frames. The white point is what maps one to the other, and it is a property
    // of the game's exposure rather than a number worth asking anyone to guess: measured means of 0.065,
    // 1.8 and 185 have all been seen in this one game.
    ++g_frames;
    TickNrRetired();
    CheckCaptureTrigger();

    if (g_captureWriteAtFrame != 0 && g_frames >= g_captureWriteAtFrame)
    {
        g_captureWriteAtFrame = 0;
        const auto captureDir = Util::DllPath().remove_filename() / "dlssnr-capture";
        const auto written = g_capture.write(captureDir);

        if (!written.empty())
            LOG_INFO("DLSS-NR wrote matched before/after frames to {}", written);
    }

    // Paper white, and nothing else. The frame is divided by this and encoded, and the soft knee
    // above 0.75 takes whatever is left over.
    //
    // It used to be divided by a white point measured from the frame -- around 3 in Cyberpunk -- which
    // was right for the old composition, where the encode had to be inverted and highlights therefore
    // had to survive it. Under the composition this now uses it is actively wrong twice over: the
    // model is handed a picture three times darker than it should see, and the highlight branch is
    // defeated. That branch hands back `originalLuma - proxyLuma`, the headroom the proxy could not
    // represent -- it exists precisely because the proxy is meant to clip. Normalising the highlights
    // away first leaves it nothing to give back.

    // Model cadence (docs/dlssnr/design/model-cadence.md): whether this frame runs the model or carries its last edit.
    // Decided here, after everything that can ask for a reset except releasing a hold -- which the hold
    // refusal covers, since a hold being released is still a hold this frame -- and before the timing
    // metadata, which says which kind of frame this is. With Cadence=1 nothing is built and every frame runs
    // the model exactly as before.
    namespace Cadence = DlssNr::Cadence;
    Cadence::Inputs cadenceInputs {};
    cadenceInputs.requested = Cadence::Requested(cfg.DlssNrCadence.value_or_default());
    cadenceInputs.driverProxy = useProxy;
    cadenceInputs.available = DlssNr_Cadence_Dx12::Available();
    cadenceInputs.presentsCounted = observedFrame != 0;
    cadenceInputs.beforeUpscale = beforeUpscale;
    cadenceInputs.gameGuides = frame.GameGuides;
    cadenceInputs.frameGeneration = FrameGenerationActive();
    cadenceInputs.allowWithFrameGen = cfg.DlssNrCadenceWithFrameGen.value_or_default();
    cadenceInputs.hold = holdFrame || g_nr.heldActive;
    cadenceInputs.capture = g_capture.isActive();
    cadenceInputs.reversibleReplace = isHdrBuffer && (cfg.DlssNrReversibleMode.value_or_default() == 2 ||
                                                      cfg.DlssNrReversibleMode.value_or_default() == 4);
    // The spread and the light limiter (docs/dlssnr/design/cadence-smoothing.md). Part of what the surfaces are built
    // for and of the settings the scheduler watches, so a toggle rebuilds them and drops the stored edit together.
    cadenceInputs.smoothing = cfg.DlssNrCadenceSmoothing.value_or_default();

    // The carry works at the model's working size, on the proxy the model is shown; without the reduced
    // proxy's surface there is no such picture to carry onto.
    const bool proxyAtWorkSize = !reduced || g_nr.colorSmall != nullptr;

    // Built only when the cadence would run, given back when it is off or cannot exist here, and otherwise kept
    // as it is, so a route or a toggle that comes and goes does not reallocate them (DlssNr_Cadence.h).
    const Cadence::Reason cadenceRefusal = Cadence::Refusal(cadenceInputs);
    if (Cadence::ReleasesSurfaces(cadenceRefusal))
    {
        ParkNrCadence(g_nr.cadence);
    }
    else if (Cadence::BuildsSurfaces(cadenceRefusal))
    {
        if (g_nr.cadence != nullptr && !g_nr.cadence->Fits(device, workWidth, workHeight, cadenceInputs.smoothing))
            ParkNrCadence(g_nr.cadence);
        if (g_nr.cadence == nullptr)
            g_nr.cadence =
                new DlssNr_Cadence_Dx12("DLSS-NR cadence", device, workWidth, workHeight, cadenceInputs.smoothing);
    }
    cadenceInputs.surfaces = proxyAtWorkSize && g_nr.cadence != nullptr && g_nr.cadence->IsInit() &&
                             g_nr.cadence->Fits(device, workWidth, workHeight, cadenceInputs.smoothing);

    // Everything whose change makes the stored edit describe another picture: what the model reads, and the
    // proxy it reads it from. Composition sliders stay out -- the edit is taken before composition.
    uint64_t cadenceSettings = Cadence::kMixSeed;
    cadenceSettings = Cadence::Mix(cadenceSettings, passSnapshot.Count);
    for (unsigned i = 0; i < passSnapshot.Count && i < passSnapshot.Settings.size(); ++i)
    {
        const auto& pass = passSnapshot.Settings[i];
        cadenceSettings = Cadence::Mix(cadenceSettings, static_cast<uint32_t>(pass.Preset));
        cadenceSettings = Cadence::Mix(cadenceSettings, static_cast<uint32_t>(pass.Style));
        cadenceSettings = Cadence::Mix(cadenceSettings, pass.Intensity);
        cadenceSettings = Cadence::Mix(cadenceSettings, pass.LocalStructure);
        cadenceSettings = Cadence::Mix(cadenceSettings, pass.LocalTone);
        cadenceSettings = Cadence::Mix(cadenceSettings, pass.SkinStructure);
        cadenceSettings = Cadence::Mix(cadenceSettings, pass.AutoMask ? 1u : 0u);
    }
    cadenceSettings = Cadence::Mix(cadenceSettings, isHdrBuffer ? 1u : 0u);
    cadenceSettings = Cadence::Mix(cadenceSettings, static_cast<uint32_t>(cfg.DlssNrReversibleMode.value_or_default()));
    cadenceSettings = Cadence::Mix(cadenceSettings, static_cast<uint32_t>(nrScalerForChain));
    cadenceSettings =
        Cadence::Mix(cadenceSettings, frame.PresentSource && cfg.DlssNrUiProtection.value_or_default() ? 1u : 0u);
    cadenceSettings = Cadence::Mix(cadenceSettings, cfg.DlssNrMatchGuides.value_or_default() ? 1u : 0u);
    cadenceSettings = Cadence::Mix(cadenceSettings, cfg.DlssNrRenderMotionScale.value_or_default() ? 1u : 0u);
    cadenceSettings = Cadence::Mix(cadenceSettings, frame.DepthInverted ? 1u : 0u);
    cadenceSettings = Cadence::Mix(cadenceSettings, frame.MotionVectorsLowResolution ? 1u : 0u);
    cadenceSettings = Cadence::Mix(cadenceSettings, frame.MvScaleX);
    cadenceSettings = Cadence::Mix(cadenceSettings, frame.MvScaleY);
    // Not something the model reads: a toggle builds the surfaces anew (Fits), and their stored edit goes with them.
    cadenceSettings = Cadence::Mix(cadenceSettings, cadenceInputs.smoothing ? 1u : 0u);

    const Cadence::Signature cadenceSignature {
        width,
        static_cast<uint32_t>(height),
        workWidth,
        workHeight,
        watchedFormat, // BGRA8 as its RGBA8 twin: a swap is the same picture, and the stored edit still describes it
        Cadence::Route(beforeUpscale, frame.AfterRayReconstruction, frame.PresentSource),
        cadenceSettings,
        g_nr.featureGeneration,
    };

    // A pending reset is held until a model frame runs: it forces one, and a carried frame never clears it.
    bool cadenceResetPending = g_nr.reset;
    for (unsigned i = 1; chainEnabled && i < passSnapshot.Count && i < 3; ++i)
        cadenceResetPending = cadenceResetPending || g_nr.passReset[i];

    const int64_t cadenceNowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const uint64_t cadenceEvaluation = ++g_cadenceEvaluations;
    const bool adaptiveRequested = cfg.DlssNrCadenceAdaptive.value_or_default() && cadenceInputs.requested > 1;
    const bool adaptiveActive = adaptiveRequested && g_tracked && cadenceInputs.surfaces &&
                                Cadence::Refusal(cadenceInputs) == Cadence::Reason::None;
    if (adaptiveActive)
    {
        for (const auto& sample : g_nr.cadence->PollProbes())
            g_cadenceAdaptive.Observe(sample, cadenceEvaluation, cadenceNowMs);
        cadenceInputs.adaptiveInterval =
            g_cadenceAdaptive.Interval(cadenceInputs.requested, cadenceEvaluation, cadenceNowMs);
    }
    else
    {
        g_cadenceAdaptive.Reset();
        if (adaptiveRequested)
            cadenceInputs.adaptiveInterval = 1;
        if (g_nr.cadence)
            g_nr.cadence->InvalidateProbe();
    }

    const auto cadence = g_cadence.Decide(
        cadenceInputs, cadenceSignature, observedFrame,
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(), cadenceResetPending);
    if (Cadence::IsForced(cadence.reason) || Cadence::IsRefusal(cadence.reason))
    {
        g_cadenceAdaptive.Reset();
        if (g_nr.cadence)
            g_nr.cadence->InvalidateProbe();
    }
    const bool carried = !cadence.runModel;
    PublishCadence(cadence, adaptiveActive ? &g_cadenceAdaptive.Last() : nullptr, cadenceEvaluation, cadenceNowMs);

    // What smoothing does on this frame, for the carry, the fade and the light limiter: the walk's share, and the
    // limiter's step for the time since the pass last ran (cadence-smoothing.md). A gap is clamped by LimitStep, and
    // a long one forced a model frame anyway, after which the limiter starts over.
    static auto cadenceLastRun = now;
    DlssNr_Cadence_Dx12::Smooth cadenceSmooth {};
    cadenceSmooth.fadeWeight = cadence.fadeWeight;
    cadenceSmooth.limit = cadence.limit;
    cadenceSmooth.limitReset = cadence.limitReset;
    cadenceSmooth.limitStep = Cadence::LimitStep(std::chrono::duration<float>(now - cadenceLastRun).count());
    cadenceSmooth.passthrough = !isHdrBuffer;
    cadenceLastRun = now;

    // A model frame not handed the chain while its history is older than one frame: the game's vectors would
    // misalign that history by every frame since, so it starts over (optimizer-fps-dlss5 fe67a80).
    if (cadence.staleHistory)
    {
        g_nr.reset = true;
        for (auto& reset : g_nr.passReset)
            reset = true;
    }

    // Transfer 2 (docs/dlssnr/design/dlss-enlargement.md): what the resolve is sent this frame, decided before the
    // timing contract so a sample says which enlargement it measured. Any other Transfer passes through unchanged and
    // touches nothing. The driver proxy never reaches the resolve, and peripheral compression hands it an edit
    // unpacked onto another grid than the working size this enlarges from, so with either the plan is matched
    // residual, which builds nothing and lets go of a private DLSS built before either was switched on.
    const auto enlargePlan =
        PlanEnlarge(cfg.DlssNrTransfer.value_or_default(), useProxy, peripheryActive,
                    cfg.DlssNrDebugView.value_or_default(), frame, device, beforeUpscale,
                    DlssNr::Enlarge::Guides(false, DlssNr_GuideMatch_Dx12::Available(), workWidth, workHeight,
                                            depthBaseX, depthBaseY, guideWidth, guideHeight, motionBaseX, motionBaseY,
                                            motionWidth, motionHeight) != DlssNr::Enlarge::GuideSource::None,
                    width, height, workWidth, workHeight, observedFrame, g_frames);

    bool timingMetadataReady = !timingEnabled;
    if (timingEnabled)
    {
        try
        {
            bool modelReset = g_nr.reset;
            for (unsigned i = 1; i < passSnapshot.Count; ++i)
                modelReset = modelReset || g_nr.passReset[i];
            timing.SetMetadata(TimingMetadata(
                cfg, passSnapshot, initialTiming.evaluationId, observedFrame, beforeUpscale ? "before" : stageForTiming,
                guideWidth, guideHeight, width, height, workWidth, workHeight, workScale, modelReset,
                g_capture.isActive() || DlssNr::PairCapture::Wanted(), isHdrBuffer, static_cast<unsigned>(desc.Format),
                cadence.cadence, carried, enlargePlan.transfer, DlssNr::Enlarge::Token(enlargePlan.why)));
            timingMetadataReady = true;
        }
        catch (...)
        {
            // Metadata failure invalidates measurement only; rendering must continue unchanged.
            timing.Reject("metadata-unavailable");
        }
    }

    // Fetch the game's exposure, where the game supplies one and the user asked for it.
    //
    // This used to measure the white point off the frame as well, over a 64x64 grid of tile
    // luminances. That is gone: the pass writes the frame it was measuring, so the divisor chased its
    // own output -- one Enshrouded session walked it from 0.010 to 97.910, and toggling NR at a fixed
    // spot read 41.31 off against 0.46 on. What remains dispatches a single thread to copy the game's
    // 1x1 exposure texture into tile 0. That is a courier, not a measurement, and cannot feed back.
    // Gated on the source the menu actually writes. This read the retired WhitePointFromExposure
    // flag while consumption keyed on WhitePointSource == 1, so choosing "the game's own exposure"
    // never dispatched the meter and the white point silently fell back to the slider.
    const bool exposureSettingOn = whitePointSource == 1;

    // Nothing held from before the option was switched off may survive switching it back on. See
    // InvalidateExposureMeter for what froze and why it read as a colour cast.
    if (DlssNr::Exposure::InvalidateOnSourceChange(g_nr.exposureSettingWasOn, whitePointSource))
    {
        InvalidateExposureMeter();
        LOG_INFO("DLSS-NR exposure: option switched on, held reading discarded");
    }
    else if (g_nr.exposureSettingWasOn && !exposureSettingOn)
    {
        InvalidateExposureMeter();
    }

    g_nr.exposureSettingWasOn = exposureSettingOn;

    ID3D12Resource* const offeredExposure = static_cast<ID3D12Resource*>(frame.ExposureTexture);
    const bool usableExposure = exposureSettingOn && ExposureTextureUsable(device, offeredExposure) &&
                                offeredExposure != source && offeredExposure != target && offeredExposure != depth &&
                                offeredExposure != motion;
    const bool holdingColor = holdFrame;
    const bool wantExposure = DlssNr::Exposure::MeterWanted(whitePointSource, usableExposure, holdingColor);
    static bool reportedInvalidExposure = false;
    if (exposureSettingOn && offeredExposure != nullptr && !usableExposure && !reportedInvalidExposure)
    {
        LOG_WARN("DLSS-NR exposure: rejected an incompatible or aliased Texture2D SRV; using CPU fallback");
        reportedInvalidExposure = true;
    }
    if (usableExposure)
        reportedInvalidExposure = false;

    g_nr.gamePreExposure = DlssNr::Exposure::PreExposure(frame.PreExposure);

    if (g_nr.meter != nullptr && wantExposure)
    {
        DlssNrConstants meterParams {};
        meterParams.Mode = DlssNrMode_Meter;

        // One pixel. Only tile (0,0) is read back, and the tile-mean branch below it in the shader is
        // dead code the dispatch simply never reaches.
        meterParams.Width = 1;
        meterParams.Height = 1;

        if (sourceIsTarget)
            Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        {
            ReadResourceScope exposureRead(cmdList, static_cast<ID3D12Resource*>(frame.ExposureTexture),
                                           exposureBarrierState);
            DispatchPass(cmdList, meterParams, source, nullptr, nullptr, (ID3D12Resource*) frame.ExposureTexture,
                         nullptr, g_nr.meter, nullptr);
        }

        if (sourceIsTarget)
            Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        CopyMeterToReadback(cmdList, device, true);
        ConsumeMeterReadback();
    }

    float whitePoint =
        ResolveWhitePoint(whitePointSource, whitePointScale, whitePointTrim, isHdrBuffer, scanInverted, scanTrim);

    // Zero-latency exposure (D3D12, source 1): when the game hands us a live exposure texture, the
    // white point is recomputed in-shader every frame from it (ExposurePreMul / exposure) instead of
    // the 3-4 frame CPU meter readback. whitePoint above still rides along in gWhitePoint as the
    // fallback the shader uses if the live sample is missing or absurd. Bound at t4 (InPrevEdit) below.
    ID3D12Resource* exposureTex = nullptr;
    uint32_t useGameExposure = 0;
    float exposurePreMul = 0.0f;

    if (DlssNr::Exposure::LiveWanted(whitePointSource, isHdrBuffer, usableExposure, holdingColor,
                                     g_nr.exposureLatch.Latched()))
    {
        exposureTex = (ID3D12Resource*) frame.ExposureTexture;
        useGameExposure = 1;
        const float trim = DlssNr::Exposure::Trim(whitePointTrim);
        exposurePreMul = g_nr.gamePreExposure * trim;
        if (!std::isfinite(exposurePreMul))
        {
            exposureTex = nullptr;
            useGameExposure = 0;
            exposurePreMul = 0.0f;
        }
    }

    // Frame hold. Freeze the encode's input so a live setting change re-renders the same frame. This
    // is self-contained on purpose: it copies the output aside on hold-on and copies it BACK over the
    // live output before the encode reads it while held, so the encode's own path and barriers below
    // are untouched and the default (hold off) is byte-identical. See docs/dlssnr/design/frame-hold.md.
    //
    // `target` is UAV here (normalised at entry, restored by the meter block above). The held copy is
    // left in COPY_SOURCE after capture and stays there for every restore.
    {
        const bool hold = holdFrame;

        if (hold)
        {
            const D3D12_RESOURCE_DESC td = target->GetDesc();

            // The held copy is one mip of one slice. CopyResource needs both sides identical, so an output
            // with mips or array slices is copied by its first subresource instead -- the one the pass
            // reads and writes. A single-subresource output keeps the whole-resource copy it always had.
            const auto copyHeld = [&](ID3D12Resource* dst, ID3D12Resource* src)
            {
                if (td.MipLevels <= 1 && td.DepthOrArraySize <= 1)
                {
                    cmdList->CopyResource(dst, src);
                    return;
                }

                D3D12_TEXTURE_COPY_LOCATION to {};
                to.pResource = dst;
                to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.SubresourceIndex = 0;

                D3D12_TEXTURE_COPY_LOCATION from {};
                from.pResource = src;
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                from.SubresourceIndex = 0;

                cmdList->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            };

            const bool needCapture = !g_nr.heldActive || g_nr.heldColor == nullptr ||
                                     (unsigned int) td.Width != g_nr.heldWidth || td.Height != g_nr.heldHeight ||
                                     td.Format != g_nr.heldFormat;

            if (needCapture)
            {
                // Hold-on (or the output changed shape under a hold): capture THIS frame, do not
                // restore -- target already holds the frame to freeze, and the pass runs on it.
                if (g_nr.heldColor != nullptr)
                    ParkNrResource(g_nr.heldColor);

                g_nr.heldColor = CreateScratch(device, td.Format, (unsigned int) td.Width, td.Height);

                if (g_nr.heldColor != nullptr)
                {
                    Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmdList, g_nr.heldColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    copyHeld(g_nr.heldColor, target);
                    Barrier(cmdList, g_nr.heldColor, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmdList, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                    g_nr.heldActive = true;
                    g_nr.heldWidth = (unsigned int) td.Width;
                    g_nr.heldHeight = td.Height;
                    g_nr.heldFormat = td.Format;
                    g_nr.heldWhitePoint = whitePoint;
                }
            }
            else
            {
                // Held: restore the frozen frame onto the live output before the encode reads it.
                Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                copyHeld(target, g_nr.heldColor);
                Barrier(cmdList, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }

            // Suspend white-point measurement while held: use the snapshot so it cannot drift and
            // confound the comparison. (No-op on the capture frame, where the snapshot IS whitePoint.)
            if (g_nr.heldActive)
                whitePoint =
                    std::isfinite(g_nr.heldWhitePoint) && g_nr.heldWhitePoint > 0.0f ? g_nr.heldWhitePoint : whitePoint;
        }
        else if (g_nr.heldActive)
        {
            // Released: let go of the frozen frame and resume live input. The model's history is of
            // the frozen frame, which this live one no longer resembles, so it starts over here.
            if (g_nr.heldColor != nullptr)
                ParkNrResource(g_nr.heldColor);
            g_nr.heldActive = false;
            g_nr.reset = true;
        }
    }

    DlssNrConstants encodeParams {};
    encodeParams.Mode = DlssNrMode_Encode;
    // A frame that is already display-referred is handed over untouched: the encode becomes a copy and
    // the resolve adds the model's edit back at full scale.
    encodeParams.Passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.WhitePoint = whitePoint;
    encodeParams.UseGameExposure = useGameExposure;
    encodeParams.ExposurePreMul = exposurePreMul;
    encodeParams.ReversibleMode = reversibleMode;
    // Match only takes effect once a fit exists; until then the table is empty and the shader would
    // read a curve of zeros, so it falls back to the plain proxy.
    encodeParams.Width = width;
    encodeParams.Height = height;

    // The encode reads the source's top-left width x height. That is the whole source on the
    // after-upscale path; on the before-upscale path the source may be a dynamic-resolution game's
    // full allocation, of which the render subrect -- the target's size -- is what was drawn.
    if (sourceIsTarget)
        Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    {
        ReadResourceScope exposureRead(cmdList, exposureTex, exposureBarrierState);
        DispatchPass(cmdList, encodeParams, source, nullptr, nullptr, nullptr, exposureTex, g_nr.colorCopy,
                     g_nr.hdrCopy);
    }

    if (sourceIsTarget)
        Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The transitions double as the wait for the encode's writes.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    RestoreResourceState colorRead { cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    // Measure the buffer's scale from the copy the encode just kept -- untouched, so there is no path
    // (Calibration pass removed: it produced only a menu suggestion nothing consumed, at the cost
    // of a 4096-thread dispatch, a readback and an nth_element every frame.)

    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    RestoreResourceState hdrRead { cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS };

    // Below full resolution the model is shown a filtered shrink of the proxy; the edit it returns is
    // enlarged during the resolve while the frame underneath stays full size and untouched.
    ID3D12Resource* modelInput = g_nr.colorCopy;

    // Without its packed surface the model would be handed the frame-size proxy as if it were packed.
    if (peripheryActive && g_nr.colorSmall == nullptr)
    {
        g_nr.peripheryFailed = true;
        g_nr.reset = true;
        LOG_ERROR("DLSS-NR periphery: the packed colour surface could not be created; off for this session");
        reportSkip("the peripheral packed colour surface could not be created");
        device->Release();
        return false;
    }

    if (reduced && g_nr.colorSmall != nullptr)
    {
        bool built = false;

        if (peripheryActive)
        {
            // Peripheral compression: the proxy packed, each packed texel the exact area average of its warped
            // footprint in the frame -- the downsample's integral with warped bounds. A pass that will not
            // record here leaves nothing usable for the model at this extent, so the frame keeps the game's
            // colour, the feature is latched off and the next frame rebuilds at the uniform size.
            if (!g_nr.periphery->PackColour(cmdList, periphery, g_nr.colorCopy, g_nr.colorSmall))
            {
                g_nr.peripheryFailed = true;
                g_nr.reset = true;
                LOG_ERROR("DLSS-NR periphery: the colour pack could not be recorded; off for this session");
                reportSkip("the peripheral colour pack could not be recorded");
                device->Release();
                return false;
            }

            Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            built = true;
        }
        else if (workScale > 1.0f)
        {
            // Supersample: enlarge the proxy to the larger working size with a real upscaling filter
            // (the Output Scaling upsampler) so the model sees a clean super-native input, rather than
            // the box minifier which only makes sense going down. colorCopy is NON_PIXEL_SHADER_RESOURCE
            // from the encode (SRV-ready); colorSmall is UNORDERED_ACCESS from last frame's resolve.
            // (Re)build the supersample scalers when missing or when the NR downscaler changed (the
            // filter is baked at construction). Both use NR's own DlssNrScalingDownscaler, independent
            // of Output Scaling, so the two can run different filters at once. superDown is built here
            // and used after the model (the down-leg below).
            if (g_nr.nrScaler != nrScalerForChain)
            {
                ParkNrScaler(g_nr.superUp);
                ParkNrScaler(g_nr.superDown);
                g_nr.nrScaler = nrScalerForChain;
            }
            if (g_nr.superUp == nullptr)
                g_nr.superUp = new OS_Dx12("DLSS-NR supersample up", device, true, nrScalerForChain);
            if (g_nr.superDown == nullptr)
                g_nr.superDown = new OS_Dx12("DLSS-NR supersample down", device, false, nrScalerForChain);

            if (g_nr.superUp != nullptr && g_nr.superUp->Dispatch(cmdList, g_nr.colorCopy, g_nr.colorSmall))
            {
                Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                built = true;
            }
        }

        if (!built)
        {
            if (workScale > 1.0f)
            {
                // Wanted to supersample but the upscaler was not available -- warn once; the box path
                // below can only enlarge blockily, so the user should know the clean path is off.
                static bool warnedSuper = false;
                if (!warnedSuper)
                {
                    warnedSuper = true;
                    LOG_WARN("DLSS-NR supersample: upscaler unavailable, falling back to a blocky enlarge.");
                }
            }

            // Sub-native (or the upsampler could not be built): box-resample the proxy to the work size.
            DlssNrConstants down {};
            down.Mode = DlssNrMode_Downsample;
            down.Width = workWidth;
            down.Height = workHeight;
            DispatchPass(cmdList, down, modelInput, nullptr, nullptr, nullptr, nullptr, g_nr.colorSmall, nullptr);
            Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        modelInput = g_nr.colorSmall;
    }

    RestoreResourceState smallRead { cmdList, reduced ? g_nr.colorSmall : nullptr,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS };

    // Read the exposure scan's candidates on the pass's own command list, once a frame.
    if (!holdingColor && !frame.PresentSource)
        DlssNr::ExposureScan::Tick(device, cmdList);

    ReadResourceScope depthRead(cmdList, depth, depthBarrierState);
    ReadResourceScope motionRead(cmdList, motion, motionBarrierState);
    ID3D12Resource* depthIn = ReadableGuide(device, cmdList, depth, &g_nr.depthClone);
    RestoreResourceState depthCloneRead { cmdList, depthIn != depth ? depthIn : nullptr,
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                          D3D12_RESOURCE_STATE_COPY_DEST };
    ID3D12Resource* motionIn = ReadableGuide(device, cmdList, motion, &g_nr.motionClone);
    RestoreResourceState motionCloneRead { cmdList, motionIn != motion ? motionIn : nullptr,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_COPY_DEST };

    if (depthIn == nullptr || motionIn == nullptr)
    {
        g_nr.failed = true;
        g_nr.reason = "the game's depth or motion vectors could not be made readable";

        // Say which guide failed and why, because the two causes need opposite answers: a guide the
        // game never supplied is not ours to fix, while a supplied one that could not be cloned is a
        // gap in TypedGuideFormat or a resource the runtime refused to create. Without this the log
        // said only that something was unreadable, which fits both.
        const auto describe = [](ID3D12Resource* src, ID3D12Resource* out) -> std::string
        {
            if (src == nullptr)
                return "not supplied by the game";

            const auto fmt = (int) src->GetDesc().Format;

            if (out != nullptr)
                return std::format("ok (format {})", fmt);

            return std::format("supplied as typeless format {}, but the typed clone could not be "
                               "created (typed format {})",
                               fmt, (int) TypedGuideFormat(src->GetDesc().Format));
        };

        LOG_ERROR("DLSS-NR unavailable: depth {} | motion vectors {}", describe(depth, depthIn),
                  describe(motion, motionIn));

        device->Release();
        return false;
    }

    // Zero-motion detection (zero-motion-detection.md): the game's own vectors, read in the game's region before
    // anything resamples them, every eighth such evaluate; the verdict is applied above, from probes already read back.
    // Only with ZeroGuideReset on: the probe is a pass of its own, and with the key off its verdict changes nothing.
    if (frame.GameGuides && cfg.DlssNrZeroGuideReset.value_or_default())
    {
        DlssNr::MotionProbe::Shape probeShape;
        probeShape.depthWidth = guideWidth;
        probeShape.depthHeight = guideHeight;
        probeShape.depthBaseX = depthBaseX;
        probeShape.depthBaseY = depthBaseY;
        probeShape.motionWidth = motionWidth;
        probeShape.motionHeight = motionHeight;
        probeShape.motionBaseX = motionBaseX;
        probeShape.motionBaseY = motionBaseY;
        probeShape.motionLowResolution = frame.MotionVectorsLowResolution;
        UpdateMotionProbe(cmdList, device, depthIn, motionIn, probeShape, cfg.DlssNrZeroGuideReset.value_or_default());
    }

    // What the model is handed as guides, and the motion-vector scale that goes with them
    // (reduced-scale-guides.md; both faults and both fixes are jlrouzies-fr's, v0.8.92 of wilsjo2's fork).
    //
    // Below the frame's size the colour shrinks to the working size. The guides used to stay at the
    // frame's, and the model does not resample a guide larger than its input: each pixel's depth and
    // motion described some other place and the history never lined up. So they are point-resampled to
    // the working size when either is larger than it, and handed over as a full zero-origin region.
    //
    // The scale used to be the game's times working / frame. The model reads it in pixels of the motion
    // texture it is handed, and the game's scale is in pixels of the size its vectors are measured in --
    // the render size for low-resolution vectors -- so that shrank every such vector below 100% whether
    // or not the guides matched. It is now the game's scale re-expressed for the texture handed over.
    ID3D12Resource* depthForModel = depthIn;
    ID3D12Resource* motionForModel = motionIn;
    unsigned int modelDepthWidth = guideWidth;
    unsigned int modelDepthHeight = guideHeight;
    unsigned int modelDepthBaseX = depthBaseX;
    unsigned int modelDepthBaseY = depthBaseY;
    unsigned int modelMotionWidth = motionWidth;
    unsigned int modelMotionHeight = motionHeight;
    unsigned int modelMotionBaseX = motionBaseX;
    unsigned int modelMotionBaseY = motionBaseY;
    bool guidesMatched = false;

    // Peripheral compression's guides are in packed space whatever their size, and their vectors in packed
    // pixels, so `guidesPacked` replaces the resample below and the model is handed a scale of 1.
    bool guidesPacked = false;

    // The game's scale is measured in original render/display pixels, even when a guide is clipped.
    const unsigned int motionReferenceWidth = DlssNr::GuideMatch::MotionReference(
        frame.MotionVectorsLowResolution, renderReferenceWidth, displayReferenceWidth);
    const unsigned int motionReferenceHeight = DlssNr::GuideMatch::MotionReference(
        frame.MotionVectorsLowResolution, renderReferenceHeight, displayReferenceHeight);

    if (peripheryActive)
    {
        if (g_nr.depthMatched == nullptr)
            g_nr.depthMatched = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, workWidth, workHeight);

        if (g_nr.motionMatched == nullptr)
            g_nr.motionMatched = CreateScratch(device, DXGI_FORMAT_R32G32_FLOAT, workWidth, workHeight);

        // The game's vectors in FRAME pixels before their ends go through the mapping: measured against the
        // size they come in, converted to the frame's. wilsjo2's v0.8.91 handed the raw game scale here,
        // render pixels for low-resolution vectors, and every packed vector came out too short.
        const float frameScaleX = DlssNr::Periphery::FrameMotionScale(g_nr.guideMvScaleX, width, motionReferenceWidth);
        const float frameScaleY =
            DlssNr::Periphery::FrameMotionScale(g_nr.guideMvScaleY, (unsigned int) height, motionReferenceHeight);

        if (g_nr.depthMatched == nullptr || g_nr.motionMatched == nullptr ||
            !g_nr.periphery->PackGuides(cmdList, periphery, depthIn, motionIn, depthBaseX, depthBaseY, guideWidth,
                                        guideHeight, motionBaseX, motionBaseY, motionWidth, motionHeight, frameScaleX,
                                        frameScaleY, g_nr.depthMatched, g_nr.motionMatched))
        {
            // The colour is already packed and nothing else fits it: no model this frame, the feature latched
            // off, and the next frame rebuilds at the uniform size.
            g_nr.peripheryFailed = true;
            g_nr.reset = true;
            LOG_ERROR("DLSS-NR periphery: the guide pack could not be recorded; off for this session");
            reportSkip("the peripheral guide pack could not be recorded");
            device->Release();
            return false;
        }

        Barrier(cmdList, g_nr.depthMatched, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmdList, g_nr.motionMatched, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        depthForModel = g_nr.depthMatched;
        motionForModel = g_nr.motionMatched;
        modelDepthWidth = modelMotionWidth = workWidth;
        modelDepthHeight = modelMotionHeight = workHeight;
        modelDepthBaseX = modelDepthBaseY = modelMotionBaseX = modelMotionBaseY = 0;
        guidesMatched = true;
        guidesPacked = true;

        static float saidFrameScale[2] = { -1.0f, -1.0f };
        if (saidFrameScale[0] != frameScaleX || saidFrameScale[1] != frameScaleY)
        {
            saidFrameScale[0] = frameScaleX;
            saidFrameScale[1] = frameScaleY;
            LOG_INFO("DLSS-NR periphery guides: depth and motion packed to {}x{}; the game's vectors to frame pixels "
                     "by {:.1f} x {:.1f} (game scale {:.1f} x {:.1f} against {}x{}, {}), then both ends through the "
                     "mapping",
                     workWidth, workHeight, frameScaleX, frameScaleY, g_nr.guideMvScaleX, g_nr.guideMvScaleY,
                     motionReferenceWidth, motionReferenceHeight,
                     frame.MotionVectorsLowResolution ? "render size, low-resolution vectors" : "output size");
        }
    }
    else if (DlssNr::GuideMatch::Wanted(cfg.DlssNrMatchGuides.value_or_default(), DlssNr_GuideMatch_Dx12::Available(),
                                        workWidth, workHeight, guideWidth, guideHeight, motionWidth, motionHeight))
    {
        if (g_nr.guideMatch == nullptr)
            g_nr.guideMatch = new DlssNr_GuideMatch_Dx12("DLSS-NR guide match", device);

        if (g_nr.depthMatched == nullptr)
            g_nr.depthMatched = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, workWidth, workHeight);

        // 32-bit float: vectors in pixels reach the thousands, where 16-bit float has lost the sub-pixel.
        if (g_nr.motionMatched == nullptr)
            g_nr.motionMatched = CreateScratch(device, DXGI_FORMAT_R32G32_FLOAT, workWidth, workHeight);

        if (g_nr.guideMatch->IsInit() && g_nr.depthMatched != nullptr && g_nr.motionMatched != nullptr &&
            g_nr.guideMatch->Dispatch(cmdList, depthIn, motionIn, depthBaseX, depthBaseY, guideWidth, guideHeight,
                                      motionBaseX, motionBaseY, motionWidth, motionHeight, g_nr.depthMatched,
                                      g_nr.motionMatched, workWidth, workHeight))
        {
            Barrier(cmdList, g_nr.depthMatched, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmdList, g_nr.motionMatched, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            depthForModel = g_nr.depthMatched;
            motionForModel = g_nr.motionMatched;
            modelDepthWidth = modelMotionWidth = workWidth;
            modelDepthHeight = modelMotionHeight = workHeight;
            modelDepthBaseX = modelDepthBaseY = modelMotionBaseX = modelMotionBaseY = 0;
            guidesMatched = true;
        }
        else
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                LOG_WARN("DLSS-NR guides: the working-size resample could not run; the model keeps the frame-size "
                         "guides this frame");
            }
        }
    }

    RestoreResourceState depthMatchedRead { cmdList, guidesMatched ? g_nr.depthMatched : nullptr,
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    RestoreResourceState motionMatchedRead { cmdList, guidesMatched ? g_nr.motionMatched : nullptr,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS };

    // Packed vectors are packed pixels already: the model's scale is 1, and RenderMotionScale has nothing to
    // choose between (the pack's own conversion above is always the corrected one).
    const bool renderMotionScale = cfg.DlssNrRenderMotionScale.value_or_default();
    const float guideMvScaleXToWork =
        guidesPacked ? 1.0f
        : renderMotionScale
            ? DlssNr::GuideMatch::ModelMotionScale(g_nr.guideMvScaleX, modelMotionWidth, motionReferenceWidth)
            : DlssNr::GuideMatch::LegacyMotionScale(g_nr.guideMvScaleX, workWidth, width);
    const float guideMvScaleYToWork =
        guidesPacked ? 1.0f
        : renderMotionScale
            ? DlssNr::GuideMatch::ModelMotionScale(g_nr.guideMvScaleY, modelMotionHeight, motionReferenceHeight)
            : DlssNr::GuideMatch::LegacyMotionScale(g_nr.guideMvScaleY, workHeight, height);
    const int guideDepthInverted = g_nr.guideDepthInverted ? 1 : 0;

    // Said once per change: which guides the model got, and the scale it was given for them.
    {
        static bool saidMatched = false;
        static bool saidPacked = false;
        static unsigned int saidShape[4] = { ~0u, ~0u, ~0u, ~0u };
        static float saidScale[2] = { -1.0f, -1.0f };
        const unsigned int shapeNow[4] = { modelMotionWidth, modelMotionHeight, workWidth, workHeight };

        if (saidMatched != guidesMatched || saidPacked != guidesPacked ||
            !std::equal(std::begin(shapeNow), std::end(shapeNow), std::begin(saidShape)) ||
            saidScale[0] != guideMvScaleXToWork || saidScale[1] != guideMvScaleYToWork)
        {
            saidMatched = guidesMatched;
            saidPacked = guidesPacked;
            std::copy(std::begin(shapeNow), std::end(shapeNow), std::begin(saidShape));
            saidScale[0] = guideMvScaleXToWork;
            saidScale[1] = guideMvScaleYToWork;

            if (guidesPacked)
                LOG_INFO("DLSS-NR guides packed by peripheral compression: depth and motion {}x{} for a {}x{} model "
                         "(the frame's guides are {}x{} and {}x{})",
                         workWidth, workHeight, workWidth, workHeight, guideWidth, guideHeight, motionWidth,
                         motionHeight);
            else if (guidesMatched)
                LOG_INFO("DLSS-NR guides matched to the working size: depth and motion {}x{} for a {}x{} model (the "
                         "frame's guides are {}x{} and {}x{})",
                         workWidth, workHeight, workWidth, workHeight, guideWidth, guideHeight, motionWidth,
                         motionHeight);
            else if (DlssNr::GuideMatch::Wanted(cfg.DlssNrMatchGuides.value_or_default(), true, workWidth, workHeight,
                                                guideWidth, guideHeight, motionWidth, motionHeight))
                LOG_INFO("DLSS-NR guides: larger than the {}x{} model and not resampled ({}); the model gets the "
                         "frame-size guides",
                         workWidth, workHeight,
                         DlssNr_GuideMatch_Dx12::Available() ? "the resample did not run"
                                                             : "this build has no guide resample shader");

            LOG_INFO("DLSS-NR model motion scale {:.1f} x {:.1f}: game scale {:.1f} x {:.1f} measured against {}x{} "
                     "({}), motion texture {}x{} ({}), model {}x{}{}",
                     guideMvScaleXToWork, guideMvScaleYToWork, g_nr.guideMvScaleX, g_nr.guideMvScaleY,
                     motionReferenceWidth, motionReferenceHeight,
                     frame.MotionVectorsLowResolution ? "render size, low-resolution vectors" : "output size",
                     modelMotionWidth, modelMotionHeight,
                     guidesPacked    ? "packed, vectors in packed pixels"
                     : guidesMatched ? "matched to the working size"
                                     : "the game's region",
                     workWidth, workHeight,
                     guidesPacked || renderMotionScale ? "" : " -- legacy working/frame conversion");
        }
    }
    const bool isLogFrame = g_frames % 120 == 0;

    // On a present source the model is also handed the back buffer -- and it reads it in its own working
    // space. Handing it the full-size frame while the model worked at 960x540 showed up as the top-left
    // quarter of the screen, HUD included, magnified 2x and blended over the picture (Generation Zero and
    // Divinity at WorkingScale 0.5 on a Windows 3060, 2026-09-24). With the matched-residual transfer a
    // wrong input would cancel out, so the magnified content could only have come in through here. It is
    // now the model's own input at the working size: the same pixels as before at scale 1 (on a present
    // source the proxy is the frame), and a match at any other scale.
    //
    // UI protection (docs/dlssnr/design/hud-protection.md): with the model built with UICorrection on, an alpha of 1
    // makes it return Backbuffer -- its own input -- in that pixel, so the edit there is zero and the
    // resolve hands back the frame as it was. The alpha is the static-overlay mask, handed as both UI and
    // UIAlpha; only a present source has interface in it.
    ID3D12Resource* uiAlpha = nullptr;
    if (frame.PresentSource && cfg.DlssNrUiProtection.value_or_default())
    {
        if (g_nr.uiMask != nullptr && !g_nr.uiMask->Fits(device, workWidth, workHeight))
            ParkNrUiMask(g_nr.uiMask);
        if (g_nr.uiMask == nullptr)
            g_nr.uiMask = new DlssNr_UiMask_Dx12("DLSS-NR UI mask", device, workWidth, workHeight);

        // Not on the model's resets: ZeroGuideReset asks for one every frame, and a mask that forgot the
        // interface every frame would never protect anything. A gap is different -- last frame's input no
        // longer describes the screen.
        const auto now = DlssNr::ReplayClock::Now();
        if (now - g_nr.uiMaskLast > std::chrono::milliseconds(250))
            g_nr.uiMask->Invalidate();
        g_nr.uiMaskLast = now;

        uiAlpha = g_nr.uiMask->Dispatch(cmdList, modelInput);
    }

    SetExtras(cfg, uiAlpha, frame.PresentSource ? modelInput : nullptr, uiAlpha != nullptr ? workWidth : 0,
              uiAlpha != nullptr ? workHeight : 0, frame.PresentSource ? workWidth : 0,
              frame.PresentSource ? workHeight : 0);

    // The proxy path, when asked for. Same inputs, same model -- the difference is who calls it.
    //
    // Nothing falls back automatically. A silent fallback would mean never finding out the proxy
    // path was broken: the picture would look right either way, because the forwarder would be
    // quietly doing the work.
    if (useProxy)
    {
        const unsigned int proxyResult =
            DlssNr::Proxy::Run(cmdList, device, modelInput, depthForModel, motionForModel, g_nr.output, workWidth,
                               workHeight, modelDepthWidth, modelDepthHeight, modelMotionWidth, modelMotionHeight,
                               modelDepthBaseX, modelDepthBaseY, modelMotionBaseX, modelMotionBaseY, guideDepthInverted,
                               g_nr.reset, guideMvScaleXToWork, guideMvScaleYToWork);

        if (coverage != nullptr)
            coverage->ModelResult(proxyResult, workWidth, workHeight);

        g_nr.reset = false;

        if (proxyResult != 1)
        {
            g_nr.failed = true;
            g_nr.reason = "the proxy path could not run the model";
            LOG_ERROR("DLSS-NR (proxy): evaluate returned 0x{:X} ({}), disabling for this session", proxyResult,
                      NgxResultName(proxyResult));
        }

        device->Release();
        return false;
    }

    // Model cadence (docs/dlssnr/design/model-cadence.md): what the carry reads this frame. The game's vectors are put
    // in working pixels by the size they are measured against, whatever the model itself is told.
    DlssNr_Cadence_Dx12::Frame cadenceFrame {};
    cadenceFrame.proxy = modelInput;
    cadenceFrame.depth = depthForModel;
    cadenceFrame.depthRegion = { modelDepthBaseX, modelDepthBaseY, modelDepthWidth, modelDepthHeight };
    cadenceFrame.motion = motionForModel;
    cadenceFrame.motionRegion = { modelMotionBaseX, modelMotionBaseY, modelMotionWidth, modelMotionHeight };
    // Under peripheral compression the guides are already packed: the vectors hold packed working pixels, scale
    // 1, as the model is told. Scaling them again from the game's units would carry every edit too far.
    cadenceFrame.mvToWorkX =
        guidesPacked ? 1.0f : DlssNr::GuideMatch::ModelMotionScale(g_nr.guideMvScaleX, workWidth, motionReferenceWidth);
    cadenceFrame.mvToWorkY =
        guidesPacked ? 1.0f
                     : DlssNr::GuideMatch::ModelMotionScale(g_nr.guideMvScaleY, workHeight, motionReferenceHeight);
    cadenceFrame.scale = Cadence::TapScale(workWidth, width);

    if (adaptiveActive)
        g_nr.cadence->Probe(
            cmdList, cadenceFrame, uiAlpha, !Cadence::IsForced(cadence.reason) && !Cadence::IsRefusal(cadence.reason),
            cadence.runModel ? !cadence.handChain : cadence.chainStart, cadenceEvaluation, cadenceNowMs);

    // The model frame after carried ones: its own history is as old as the last model frame, so it is handed the
    // chain summed back to it -- working pixels, scale 1, off the picture where the chain broke -- rather than
    // this frame's vectors, which would misalign that history by every carried frame (BeliyG3's lesson). The
    // texture has the shape the guide match already hands over. A forced model frame keeps the game's vectors.
    float modelMvScaleX = guideMvScaleXToWork;
    float modelMvScaleY = guideMvScaleYToWork;
    bool cadenceChainHanded = false; // the chain was handed: a smoothed model frame can fade in along it
    if (cadence.handChain && g_nr.cadence != nullptr)
    {
        if (ID3D12Resource* chainMotion = g_nr.cadence->ModelMotion(cmdList, cadenceFrame))
        {
            cadenceChainHanded = true;
            motionForModel = chainMotion;
            modelMotionWidth = workWidth;
            modelMotionHeight = workHeight;
            modelMotionBaseX = modelMotionBaseY = 0;
            modelMvScaleX = modelMvScaleY = 1.0f;
        }
        else
        {
            // No chain to hand, and the game's vectors would misalign that history the same way.
            g_nr.reset = true;
            for (auto& reset : g_nr.passReset)
                reset = true;
        }
    }

    int result = 0;
    DlssNr::Chain::Routing<ID3D12Resource*> chain(modelInput, g_nr.output, g_nr.passPing);

    if (carried)
    {
        // A carried frame: no model call and no extra pass. The proxy plus the last edit, moved along the chain,
        // is written where the model writes its answer, and the resolve below composes it as one. The pending
        // reset, if any, is held -- there is none here, since one forces a model frame -- and the menu keeps
        // showing what the last model frame completed. With smoothing, the walk's share of the last fade comes with
        // it, and the light limiter holds what is shown.
        const bool carriedOk = g_nr.cadence != nullptr && g_nr.cadence->Carry(cmdList, cadenceFrame, cadence.chainStart,
                                                                              uiAlpha, cadenceSmooth, g_nr.output);
        result = carriedOk ? 1 : 0; // 1 is NVSDK_NGX_Result_Success: the resolve composes it like an answer
        g_activePasses = g_nr.cadencePasses;
    }
    else
    {
        const bool profileKernels = cfg.DlssNrKernelProfile.value_or_default();
        if (profileKernels)
            DlssNr::KernelProfile::Profiler::Instance().Begin(cmdList, true);

        timing.ModelBegin();

        // Preserve the single/master call and anchor. Each extra layer has independent history.
        const auto firstCpuStart =
            chainEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
        result = g_nr.evaluate(cmdList, g_nr.feature, g_nr.capabilityParams, modelInput, depthForModel, motionForModel,
                               g_nr.output, workWidth, workHeight, modelDepthWidth, modelDepthHeight, modelMotionWidth,
                               modelMotionHeight, modelDepthBaseX, modelDepthBaseY, modelMotionBaseX, modelMotionBaseY,
                               guideDepthInverted, g_nr.reset ? 1 : 0, intensityForChain, styleForChain,
                               localStructureForChain, localToneForChain, skinStructureForChain,
                               autoMaskForChain ? 1 : 0, modelMvScaleX, modelMvScaleY);

        timing.ModelEnd();

        if (profileKernels)
        {
            auto& kernelProfiler = DlssNr::KernelProfile::Profiler::Instance();
            kernelProfiler.End(cmdList);
            for (const auto& rep : kernelProfiler.TakeReports())
                LOG_INFO("{}", rep);
        }

        if (chainEnabled && (isLogFrame || result != 1))
            LOG_INFO(
                "DLSS-NR chain: pass 1 result=0x{:X}, dimensions={}x{}, CPU-call-ms={:.3f} (not GPU time)",
                (unsigned) result, workWidth, workHeight,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - firstCpuStart).count());
        if (result == 1)
        {
            chain.Success();
            DlssNr::SessionGuardEvaluated();
        }
        if (chainEnabled && result == 1)
        {
            for (unsigned i = 1; i < passSnapshot.Count; ++i)
            {
                if (!g_nr.passFeature[i] || g_nr.passFailed[i] || observedFrame <= g_nr.creationFrame[i] ||
                    !DlssNr::Submission::Completed(g_nr.creation[i]))
                {
                    ChainStatus(g_nr.passFailed[i]
                                    ? "Additional pass failed/admission refused; last successful answer retained"
                                    : "Additional pass waiting for creation and submitted GPU completion; last "
                                      "successful answer retained");
                    break;
                }
                auto* input = chain.Input();
                auto* answer = chain.Output();
                Barrier(cmdList, input, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                const auto& settings = passSnapshot.Settings[i];
                SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
                const auto startCpu = std::chrono::steady_clock::now();
                timing.ModelBegin();
                const int extraResult = g_nr.evaluate(
                    cmdList, g_nr.passFeature[i], g_nr.capabilityParams, input, depthForModel, motionForModel, answer,
                    workWidth, workHeight, modelDepthWidth, modelDepthHeight, modelMotionWidth, modelMotionHeight,
                    modelDepthBaseX, modelDepthBaseY, modelMotionBaseX, modelMotionBaseY, guideDepthInverted,
                    (g_nr.reset || g_nr.passReset[i]) ? 1 : 0, settings.Intensity, (int) settings.Style,
                    settings.LocalStructure, settings.LocalTone, settings.SkinStructure, settings.AutoMask ? 1 : 0,
                    modelMvScaleX, modelMvScaleY);
                timing.ModelEnd();
                const double cpuMs =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startCpu).count();
                Barrier(cmdList, input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (extraResult != 1)
                {
                    g_nr.passFailed[i] = true;
                    ParkNrFeature(g_nr.passFeature[i]);
                    for (unsigned downstream = i; downstream < 3; ++downstream)
                        g_nr.passReset[downstream] = true;
                    LOG_INFO("DLSS-NR chain: pass {} failed result=0x{:X}, completed={}, CPU-call-ms={:.3f}; "
                             "preserving previous answer",
                             i + 1, (unsigned) extraResult, chain.completed, cpuMs);
                    break;
                }
                g_nr.passReset[i] = false;
                chain.Success();
                if (isLogFrame)
                    LOG_INFO("DLSS-NR chain: pass {} evaluated at {}x{}, CPU-call-ms={:.3f} (not GPU time)", i + 1,
                             workWidth, workHeight, cpuMs);
            }
        }
        g_activePasses = chain.completed;
        g_nr.cadencePasses = chain.completed;
        if (chainEnabled && chain.completed == passSnapshot.Count)
            ChainStatus("All requested passes completed; one final composition");
        if (chainEnabled && (isLogFrame || chain.completed != passSnapshot.Count))
        {
            static unsigned previousRequested = 0, previousCompleted = 0;
            if (isLogFrame || previousRequested != passSnapshot.Count || previousCompleted != chain.completed)
                LOG_INFO("DLSS-NR chain: requested={} completed={} present={} stage={} status={}", passSnapshot.Count,
                         chain.completed, observedFrame, beforeUpscale ? "before" : "after", g_chainStatus.load());
            previousRequested = passSnapshot.Count;
            previousCompleted = chain.completed;
        }

        if (coverage != nullptr)
            coverage->ModelResult(result, workWidth, workHeight);
    }

    // On a carried frame the answer is the synthesized one, in the surface the model writes.
    ID3D12Resource* finalOutput = carried ? g_nr.output : chain.answer;

    // Read before it is cleared: everything that reset the model this frame resets the stabilizer too,
    // except the zero-guide policy, which changes nothing on screen.
    const bool stabilizerReset = frameResetIsPolicy ? resetBeforeFrameFlag : g_nr.reset;

    // Cleared only by a frame the model ran on. A carried frame holds it, so a reset is never lost on one.
    if (!carried)
        g_nr.reset = false;

    // Whether this model frame's edit was stored for the frames that carry it, and whether it was shown faded in from
    // the carried edit, under the light limiter (cadence-smoothing.md).
    bool cadenceRecorded = false;
    bool cadenceFaded = false;

    // Supersampling probe: report the model working ABOVE native so a test log tells us whether NGX even
    // accepts a super-native evaluate and what it returns. Once per working-size change, or on any error.
    if (workWidth > width || workHeight > height)
    {
        static unsigned int lastSuper = 0;
        if (lastSuper != workWidth || result != 1)
        {
            lastSuper = workWidth;
            LOG_INFO("DLSS-NR SUPERSAMPLE: model at {}x{} = {:.2f}x native {}x{}, evaluate result {} ({})", workWidth,
                     workHeight, (float) workWidth / (float) width, width, height, result,
                     NgxResultName((unsigned int) result));
        }
    }

    // Once, a few seconds in, so it lands after the values have been written at least once.
    static bool tuningReported = false;

    if (!tuningReported && g_frames > 240)
    {
        tuningReported = true;

        // At INFO, because whether the model actually took a value is the only way to tell a
        // control that does nothing from one that is not being written.
        auto report = [](const char* name, float wrote)
        {
            float value = 0.0f;
            const NVSDK_NGX_Result r = g_nr.capabilityParams->Get(name, &value);
            LOG_INFO("DLSS-NR readback {} -> {} (we wrote {}, result 0x{:X})", name, value, wrote, (uint32_t) r);
        };

        const Config& rcfg = *Config::Instance();
        report("DLSSNR.Intensity", rcfg.DlssNrIntensity.value_or_default());
        report("DLSSNR.LocalStructureStrength", rcfg.DlssNrLocalStructure.value_or_default());
        report("DLSSNR.LocalToneStrength", rcfg.DlssNrLocalTone.value_or_default());
        report("DLSSNR.SkinStructureStrength", rcfg.DlssNrSkinStructure.value_or_default());

        unsigned int style = 0;
        const NVSDK_NGX_Result styleResult = g_nr.capabilityParams->Get("DLSSNR.Style", &style);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Style -> {} (result 0x{:X})", style, (uint32_t) styleResult);

        // The preset is the last control whose arrival has never been checked, and three of them look
        // identical in play. Either it is not landing or the presets really are alike.
        unsigned int preset = 0;
        const NVSDK_NGX_Result presetResult = g_nr.capabilityParams->Get("DLSSNR.Hint.Render.Preset", &preset);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Hint.Render.Preset -> {} (result 0x{:X}, we wrote {})", preset,
                  (uint32_t) presetResult, cfg.DlssNrPreset.value_or_default());

        LOG_DEBUG("DLSS-NR wrote intensity {}, local structure {}, local tone {}, skin {}, style {}",
                  cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                  cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                  cfg.DlssNrStyle.value_or_default());
    }

    if (result == NVSDK_NGX_Result_Success)
    {
        // Resolve takes the difference between what the model returned and what it was shown, and adds
        // that back to the frame. At strength zero the result is what the upscaler produced, exactly, and
        // anything the model left alone is untouched rather than round-tripped through the curve.
        DlssNrConstants resolveParams {};
        resolveParams.Mode = DlssNrMode_Resolve;
        resolveParams.WhitePoint = whitePoint;
        resolveParams.UseGameExposure = useGameExposure;
        resolveParams.ExposurePreMul = exposurePreMul;
        resolveParams.Width = width;
        resolveParams.Height = height;
        resolveParams.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
        resolveParams.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
        resolveParams.DebugView = cfg.DlssNrDebugView.value_or_default();
        resolveParams.MaxRatio = cfg.DlssNrMaxRatio.value_or_default();
        // The configured value, except that Transfer 2 is sent only on frames its DLSS runs (enlargePlan).
        resolveParams.Transfer = enlargePlan.transfer;
        resolveParams.DebugScale = cfg.DlssNrWhitePointScale.value_or_default();
        resolveParams.Passthrough = isHdrBuffer ? 0u : 1u;
        resolveParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
        resolveParams.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
        resolveParams.CompareMode = cfg.DlssNrCompare.value_or_default();
        resolveParams.CompareSplit = cfg.DlssNrCompareSplit.value_or_default();
        resolveParams.CompareZoom = std::max(1.0f, cfg.DlssNrCompareZoom.value_or_default());
        resolveParams.CompareSwap = cfg.DlssNrCompareSwap.value_or_default() ? 1u : 0u;
        // Only where the encode compressed anything: on a frame the game already tone mapped the model saw the
        // frame as it is (docs/dlssnr/design/hdr-highlight-recovery.md). The shader gates on passthrough as well.
        resolveParams.HighlightRecovery = isHdrBuffer && cfg.DlssNrHighlightRecovery.value_or_default() ? 1u : 0u;
        // The residual limit, in stops (docs/dlssnr/design/residual-limit.md), clamped to what its control offers so a
        // hand-edited ini cannot hand the shader a negative or a NaN. 0 is off.
        resolveParams.ResidualLimit = DlssNr::ResidualLimitStops(cfg.DlssNrResidualLimit.value_or_default());
        resolveParams.MinRatio = DlssNr::MinimumRatio(cfg.DlssNrMinRatio.value_or_default());

        // The output stabilizer's two numbers, clamped to what its controls offer, so a hand-edited ini
        // cannot hand the shader a negative strength or a NaN. Read here because the composition report
        // below carries them: a screenshot taken with the stabilizer on must be tellable from a bug.
        const float stabilizerStrengthRaw = cfg.DlssNrStabilizerStrength.value_or_default();
        const float stabilizerStrength =
            std::isfinite(stabilizerStrengthRaw) ? std::clamp(stabilizerStrengthRaw, 0.0f, 1.0f) : 0.0f;
        const float stabilizerToleranceRaw = cfg.DlssNrStabilizerTolerance.value_or_default();
        const float stabilizerTolerance =
            std::isfinite(stabilizerToleranceRaw) ? std::clamp(stabilizerToleranceRaw, 0.01f, 0.2f) : 0.05f;

        // The numbers the composition actually ran with, logged when any of them changes.
        //
        // A colour report without these cannot be read. Paper white alone decides whether the model
        // was shown a sensible picture or a blown one, and it was absent from every log in the first
        // round of reports -- one tester's "much better at 16" had to be taken on trust because
        // nothing in the file said what the value was. Debug view and compare mode are here for the
        // same reason from the other direction: both change what is on screen, and a screenshot with
        // one left on is indistinguishable from a bug.
        struct ComposeReport
        {
            float whitePoint;
            float transfer;
            float colour;
            float maxRatio;
            float residualLimit;
            float minRatio;
            unsigned int highlightRecovery;
            unsigned int passthrough;
            unsigned int debugView;
            unsigned int compareMode;
            unsigned int residual;
            unsigned int workW;
            unsigned int workH;
            int whiteSource;
            float whiteScale;
            float whiteTrim;
            bool scanInverted;
            float scanTrim;
            bool scanEnabled;
            bool hold;
            float stabilizer;
            float stabilizerTolerance;
            std::vector<DlssNr::ExposureScan::AnchorPoint> anchors;

            bool SameAs(const ComposeReport& other) const { return whitePoint == other.whitePoint && SameShape(other); }

            // The computed white point can drift OR change because a control moved.
            // Keep its configuration separately so manual edits/reversals never wait.
            bool SameShape(const ComposeReport& other) const
            {
                return transfer == other.transfer && colour == other.colour && maxRatio == other.maxRatio &&
                       residualLimit == other.residualLimit && minRatio == other.minRatio &&
                       highlightRecovery == other.highlightRecovery && passthrough == other.passthrough &&
                       debugView == other.debugView && compareMode == other.compareMode && residual == other.residual &&
                       workW == other.workW && workH == other.workH && whiteSource == other.whiteSource &&
                       whiteScale == other.whiteScale && whiteTrim == other.whiteTrim &&
                       scanInverted == other.scanInverted && scanTrim == other.scanTrim &&
                       scanEnabled == other.scanEnabled && hold == other.hold && stabilizer == other.stabilizer &&
                       stabilizerTolerance == other.stabilizerTolerance &&
                       std::equal(anchors.begin(), anchors.end(), other.anchors.begin(), other.anchors.end(),
                                  [](const auto& a, const auto& b) { return a.scan == b.scan && a.white == b.white; });
            }
        };

        static DlssNr::LogRate::Reporter<ComposeReport> composeReporter;

        // Quantised to the precision it is printed at. Comparing raw floats logged 2376 lines in one
        // Enshrouded session, because a measured white point drifts continuously and every drift was a
        // change. A line per meaningful change is the point; a line per frame is a different problem.
        //
        // Quantisation is a precision limit, not a rate limit. A measured white point still crosses a
        // 0.01 boundary about nine times a second, which is what the 486 composition lines of one
        // Crimson Desert session contained, alongside one model-size change. The Reporter holds
        // measured drift to one line a second; configuration and dimension changes pass immediately.
        // Source-specific controls are normalized out when inactive. docs/dlssnr/design/nr-log-rate.md.
        const ComposeReport composeNow {
            std::round(resolveParams.WhitePoint * 100.0f) / 100.0f,
            resolveParams.TransferStrength,
            resolveParams.ColourStrength,
            resolveParams.MaxRatio,
            resolveParams.ResidualLimit,
            resolveParams.MinRatio,
            resolveParams.HighlightRecovery,
            resolveParams.Passthrough,
            resolveParams.DebugView,
            resolveParams.CompareMode,
            resolveParams.Transfer,
            g_nr.workWidth,
            g_nr.workHeight,
            whitePointSource,
            std::isfinite(whitePointScale) && whitePointScale > 0.0f ? whitePointScale : 1.0f,
            whitePointSource == 1 ? DlssNr::Exposure::Trim(whitePointTrim) : 1.0f,
            whitePointSource == 2 && scanInverted,
            whitePointSource == 2 ? std::clamp(scanTrim, 0.01f, 4.0f) : 1.0f,
            whitePointSource == 2 && cfg.DlssNrScanExposure.value_or_default(),
            holdFrame,
            stabilizerStrength,
            stabilizerStrength > 0.0f ? stabilizerTolerance : 0.0f,
            whitePointSource == 2 ? DlssNr::ExposureScan::Anchors() : std::vector<DlssNr::ExposureScan::AnchorPoint>()
        };

        if (composeReporter.Observe(composeNow, DlssNr::LogRate::Clock::now()))
        {
            LOG_INFO("DLSS-NR composition: paper white {:.2f}x, detail {:.2f}, colour {:.2f}, guard "
                     "{:.1f}x, residual limit {}, minimum brightness {:.2f}x, highlight recovery {}, colour transform "
                     "{}, transfer {}, model {}x{}, "
                     "debug view {}, compare {}, stabilizer {:.2f} (tolerance {:.2f})",
                     composeNow.whitePoint, composeNow.transfer, composeNow.colour, composeNow.maxRatio,
                     composeNow.residualLimit > 0.0f ? std::format("{:.2f} stops", composeNow.residualLimit)
                                                     : std::string("off"),
                     composeNow.minRatio, composeNow.highlightRecovery != 0 ? "on" : "off",
                     composeNow.passthrough != 0 ? "off (frame already tone mapped)" : "on (linear HDR)",
                     composeNow.residual == 1   ? "matched residual"
                     : composeNow.residual == 2 ? "matched residual + DLSS"
                     : composeNow.residual == 3 ? "matched residual, sharp"
                                                : "classic",
                     composeNow.workW, composeNow.workH, composeNow.debugView, composeNow.compareMode,
                     composeNow.stabilizer, composeNow.stabilizerTolerance);
        }

        Barrier(cmdList, finalOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // Model cadence: a model frame stores its final answer minus its proxy, the proxy and the depth for the
        // frames that carry it -- only when every requested pass completed, so a carried frame never shows a
        // partial chain's answer, and only when the scheduler asks, so a refused or switched-off cadence, or a
        // second call in one present, records nothing.
        //
        // With smoothing, a scheduled model frame first stores the carried answer minus its own -- reading the stored
        // edit the record is about to replace -- and keeps its answer with the walk's first share of the carried one;
        // after the record, that shown answer goes, under the light limiter, where the resolve reads. The record and
        // the model's own history keep the model's answer as it is: smoothing is what is shown, never what is kept.
        const bool cadenceFading = cadence.fade && cadenceChainHanded && cadence.record && !carried &&
                                   chain.completed == passSnapshot.Count && g_nr.cadence != nullptr &&
                                   g_nr.cadence->Fade(cmdList, cadenceFrame, uiAlpha, finalOutput, cadenceSmooth);
        if (cadence.record && !carried && chain.completed == passSnapshot.Count && g_nr.cadence != nullptr)
            cadenceRecorded = g_nr.cadence->Record(cmdList, cadenceFrame, finalOutput);
        if (cadenceFading && cadenceRecorded)
        {
            Barrier(cmdList, finalOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cadenceFaded = g_nr.cadence->ShowFaded(cmdList, cadenceFrame, uiAlpha, cadenceSmooth, finalOutput);
            Barrier(cmdList, finalOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        // Supersampling down-leg. Average the Nx model answer back to native with the chosen filter, so
        // the resolve composites a native answer against the native proxy 1:1 -- a real area resample,
        // not the single bilinear tap the Nx answer would otherwise get in the resolve (which aliases
        // the model's detail into noise, the "noisier above 100%" the probe showed). On success the
        // resolve reads the native proxy (colorCopy) and native answer (outputNative); on failure it
        // falls back to the Nx pair. g_nr.output is NPSR here; outputNative is UAV from last frame.
        bool superDownOk = false;
        if (workScale > 1.0f && g_nr.superDown != nullptr && g_nr.outputNative != nullptr &&
            g_nr.superDown->Dispatch(cmdList, finalOutput, g_nr.outputNative))
        {
            Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            superDownOk = true;
        }

        const auto resolved =
            DlssNr::Chain::Resolve(g_nr.colorCopy, modelInput, g_nr.outputNative, finalOutput, superDownOk);
        ID3D12Resource* resolveProxy = resolved.proxy;
        ID3D12Resource* resolveAnswer = resolved.answer;

        // Peripheral compression (docs/dlssnr/design/peripheral-compression.md): the packed proxy and the packed answer
        // brought back onto the uniform grid, once, after every pass of the chain, so the resolve reads them
        // exactly as it reads the uniform path's pair. ForceResidual keeps Transfer 1 and 3 on the residual
        // path when that grid is the frame's own size, where the size test alone would fall back to Classic
        // over a proxy that has been packed and unpacked.
        bool unpacked = false;
        bool resolvable = true;

        if (peripheryActive)
        {
            if (g_nr.unpackedProxy == nullptr)
                g_nr.unpackedProxy = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, gridWidth, gridHeight);
            if (g_nr.unpackedAnswer == nullptr)
                g_nr.unpackedAnswer = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, gridWidth, gridHeight);

            if (g_nr.unpackedProxy != nullptr && g_nr.unpackedAnswer != nullptr &&
                g_nr.periphery->Unpack(cmdList, periphery, modelInput, finalOutput, g_nr.unpackedProxy,
                                       g_nr.unpackedAnswer, gridWidth, gridHeight))
            {
                Barrier(cmdList, g_nr.unpackedProxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cmdList, g_nr.unpackedAnswer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                resolveProxy = g_nr.unpackedProxy;
                resolveAnswer = g_nr.unpackedAnswer;
                resolveParams.ForceResidual = 1;
                unpacked = true;
            }
            else
            {
                // A packed answer composed as if it were uniform would put the whole periphery in the wrong
                // place, so this frame keeps the game's colour, and the next rebuilds at the uniform size.
                g_nr.peripheryFailed = true;
                g_nr.reset = true;
                resolvable = false;
                LOG_ERROR("DLSS-NR periphery: the unpack could not be recorded; off for this session");
            }
        }

        // Debug view 4, "Packed model input": the model's input as it was handed over, shown through the
        // proxy view. Only while the periphery ran: otherwise 4 reaches the shader as it always did, which
        // knows no view 4 and composes the frame -- the default path stays what it was for every value.
        if (resolveParams.DebugView == 4 && unpacked)
        {
            resolveProxy = modelInput;
            resolveParams.DebugView = 1;
        }

        // Transfer 2 (docs/dlssnr/design/dlss-enlargement.md): the edit enlarged by the private DLSS, which is also
        // built here on the frame it is first wanted. When it does not run, the resolve is sent matched residual.
        ID3D12Resource* enlarged = nullptr;
        if (enlargePlan.keep)
        {
            EnlargeFrame ef;
            ef.proxy = resolveProxy;
            ef.answer = resolveAnswer;
            ef.matchedDepth = guidesMatched ? g_nr.depthMatched : nullptr;
            ef.matchedMotion = guidesMatched ? g_nr.motionMatched : nullptr;
            ef.gameDepth = depthIn;
            ef.gameMotion = motionIn;
            ef.depthBaseX = depthBaseX;
            ef.depthBaseY = depthBaseY;
            ef.depthWidth = guideWidth;
            ef.depthHeight = guideHeight;
            ef.motionBaseX = motionBaseX;
            ef.motionBaseY = motionBaseY;
            ef.motionWidth = motionWidth;
            ef.motionHeight = motionHeight;
            ef.workWidth = workWidth;
            ef.workHeight = workHeight;
            ef.width = width;
            ef.height = height;
            ef.depthInverted = frame.DepthInverted;
            ef.mvScaleX = DlssNr::Enlarge::MotionScale(g_nr.guideMvScaleX, workWidth, motionReferenceWidth, holdFrame);
            ef.mvScaleY =
                DlssNr::Enlarge::MotionScale(g_nr.guideMvScaleY, workHeight, motionReferenceHeight, holdFrame);
            ef.passthrough = resolveParams.Passthrough != 0;
            ef.reset = stabilizerReset;
            ef.hold = holdFrame;
            ef.present = observedFrame;
            ef.frame = g_frames;

            enlarged = EnlargeEdit(*this, cmdList, device, enlargePlan, ef);

            if (enlarged != nullptr)
            {
                resolveAnswer = enlarged;
            }
            else if (enlargePlan.evaluate)
            {
                // Planned as 2 and did not run: matched residual this frame, and no timing sample for a
                // contract that says DLSS ran.
                resolveParams.Transfer = DlssNr::Enlarge::kTransferMatched;
                timing.Reject("enlarge-fallback");
            }
        }

        if (resolvable)
        {
            ReadResourceScope exposureRead(cmdList, exposureTex, exposureBarrierState);
            wrote = DispatchPass(cmdList, resolveParams, resolveProxy, resolveAnswer, g_nr.hdrCopy, motionIn,
                                 exposureTex, target, nullptr);
        }
        Barrier(cmdList, finalOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (enlarged != nullptr)
            Barrier(cmdList, enlarged, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (superDownOk)
            Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (unpacked)
        {
            Barrier(cmdList, g_nr.unpackedProxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cmdList, g_nr.unpackedAnswer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        // The output stabilizer (docs/dlssnr/design/output-stabilizer.md), last, over what the resolve just wrote.
        //
        // Only where the pass edits the frame in place. Before the upscaler the input is the jittered
        // render-size frame about to go into it: jitter reads as change on every pixel, and a held pixel
        // would hand the upscaler a stale jittered sample to accumulate.
        //
        // The gate reads hdrCopy, the frame as the upscaler left it, scaled by the white point the
        // composition used -- not colorCopy, the model's proxy, whose knee flattens every stop above white
        // while the resolve takes those highlights from hdrCopy. And only over a frame the resolve actually
        // wrote; otherwise there is no answer to hold. Off and before-upscale were parked at the top.
        if (stabilizerStrength > 0.0f && !beforeUpscale && wrote)
        {
            // HDR10 at present: the creep's unit is a PQ code, the format the frame goes out in, not the work
            // texture's.
            const auto outputDesc = target->GetDesc();
            if (g_nr.stabilizer != nullptr && !g_nr.stabilizer->Fits(device, outputDesc, frame.PresentQuantRel))
                ParkNrStabilizer(g_nr.stabilizer);
            if (g_nr.stabilizer == nullptr)
                g_nr.stabilizer =
                    new DlssNr_Stabilizer_Dx12("DLSS-NR stabilizer", device, outputDesc, frame.PresentQuantRel);

            // What else drops its history, beyond what reset the model. Any setting that changes the
            // composed picture -- at strength 1 a still pixel never takes a new answer, so without this a
            // slider moved on a still scene would seem to do nothing, and HoldFrame + ApplyModel, the A/B
            // pair, would compare a frame with itself. And a gap: the pass or the stabilizer was off, or
            // the stage was before the upscaler, so the history no longer describes the screen.
            static std::optional<ComposeReport> stabilizerShape;
            static DlssNrPassSnapshot stabilizerPasses {};
            // The periphery's layout too: a centre moved at the same packed extent reshapes every edge pixel.
            static std::tuple<float, float, uint32_t, uint32_t, uint32_t, bool, Scaler, uint32_t, uint32_t>
                stabilizerExtras {};
            const auto extrasNow = std::make_tuple(
                resolveParams.CompareSplit, resolveParams.CompareZoom, resolveParams.CompareSwap,
                resolveParams.ReversibleMode, resolveParams.ApplyModel, frame.AfterRayReconstruction, nrScalerForChain,
                peripheryActive ? periphery.settings.center : 0u, peripheryActive ? periphery.settings.work : 0u);
            const bool settingsChanged = !stabilizerShape || !stabilizerShape->SameShape(composeNow) ||
                                         stabilizerPasses.Count != passSnapshot.Count ||
                                         stabilizerPasses.Individual != passSnapshot.Individual ||
                                         stabilizerPasses.Settings != passSnapshot.Settings ||
                                         stabilizerExtras != extrasNow;
            const auto now = DlssNr::ReplayClock::Now();
            const bool lapsed = now - g_nr.stabilizerLast > std::chrono::milliseconds(250);

            if (stabilizerReset || settingsChanged || lapsed)
                g_nr.stabilizer->Invalidate();

            stabilizerShape = composeNow;
            stabilizerPasses = passSnapshot;
            stabilizerExtras = extrasNow;
            g_nr.stabilizerLast = now;

            g_nr.stabilizer->Dispatch(cmdList, g_nr.hdrCopy, target, stabilizerStrength, stabilizerTolerance,
                                      resolveParams.WhitePoint, resolveParams.Passthrough != 0);
        }

        // On-demand capture works in this path too: the staging copy still holds the frame as the
        // upscaler produced it, and the edited frame is the output itself. The write happens a few
        // frames later, once the GPU is certainly past these copies -- this path has no fence of its
        // own.
        if (g_capture.isActive())
        {
            // "Before" is hdrCopy, the encode's keep: the frame value for value, the one the pass read (the held
            // frame while Hold is on), on every route. The in-place routes used to record colorCopy, which is the
            // proxy the model was shown -- soft-knee and sRGB-encoded for a linear frame -- so "before" was not the
            // frame the manifest says it is (docs/dlssnr/design/comparison-capture.md, open question 4).
            g_capture.record(cmdList, device, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, target,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            if (g_capture.readyToWrite() && g_captureWriteAtFrame == 0)
                g_captureWriteAtFrame = g_frames + 8;
        }

        // The comparison capture: the frame the pass read and the frame it wrote, copied on this list after the last
        // write to the target, so the pair is one frame by construction (docs/dlssnr/design/comparison-capture.md).
        // Only a frame the resolve wrote. Idle, one relaxed load; the description is built only when one is wanted.
        // Describing the frame allocates; like the timing metadata above, a throw must not leave the frame, so the
        // capture is withdrawn with a reason instead.
        if (wrote && DlssNr::PairCapture::Wanted())
        {
            try
            {
                DlssNr::PairCapture::FrameFacts facts;
                facts.route = frame.PresentSource            ? (frame.BridgeHost ? "d3d11-bridge" : "d3d12-present")
                              : beforeUpscale                ? "before-upscale"
                              : frame.AfterRayReconstruction ? "after-rr"
                                                             : "after-upscale";
                facts.surface = frame.PresentSource ? "present-final"
                                : beforeUpscale     ? "render-input"
                                                    : "upscaler-output";
                facts.hookMethod = cfg.DlssNrHookMethod.value_or_default();
                facts.held = g_nr.heldActive;
                facts.carried = carried;
                facts.linearHdr = isHdrBuffer;
                facts.proxy = useProxy;
                facts.compare = cfg.DlssNrCompare.value_or_default();
                facts.debugView = cfg.DlssNrDebugView.value_or_default();
                facts.whitePoint = resolveParams.WhitePoint;
                facts.frame = g_frames;
                facts.present = observedFrame;
                facts.settings = TimingContract(
                    cfg, passSnapshot, beforeUpscale ? "before" : stageForTiming, guideWidth, guideHeight, width,
                    height, workWidth, workHeight, workScale, isHdrBuffer, static_cast<unsigned>(desc.Format),
                    cadence.cadence, enlargePlan.transfer, DlssNr::Enlarge::Token(enlargePlan.why));
                DlssNr::PairCapture::AtRecordSite(cmdList, device, g_nr.hdrCopy,
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, target,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, facts);
            }
            catch (const std::exception& e)
            {
                DlssNr::PairCapture::AbandonAfterError(e.what());
            }
            catch (...)
            {
                DlssNr::PairCapture::AbandonAfterError(nullptr);
            }
        }
    }
    else if (carried)
    {
        // The carry could not be recorded. The model was not asked for anything, so nothing here says the model
        // failed; the frame keeps the game's colour and the next one runs the model.
        reportSkip("model cadence could not carry the edit this frame");
    }
    else if (result == 0xBAD00005)
    {
        // FAIL_InvalidParameter is the model rejecting what it was asked for, not the model breaking.
        //
        // Measured in Divinity: Original Sin 2 at 3440x1440 by raising the working scale to 200%,
        // which asks for 6880x2880 -- 19.8 Mpx, four times anything this project has ever measured
        // (model-cost-vs-working-scale.md stops at 1.0). The model refuses it. Every refusal in that
        // session arrived within ten lines of a rebuild and never during steady operation, which is
        // what a size the model will not take looks like.
        //
        // Treating that as session-fatal punished the user for moving a slider: the pass went dead
        // and stayed dead, and putting the slider back did nothing until the next rebuild happened to
        // clear it. The parameter is theirs to change, so the refusal is theirs to undo. Remember the
        // size that was refused, keep the pass alive, and decline only that size.
        if (!g_nr.rejectedWork || g_nr.rejectedWorkWidth != workWidth || g_nr.rejectedWorkHeight != workHeight)
        {
            LOG_WARN("DLSS-NR: the model refused {}x{} (0x{:X} {}). The pass stays available; change the working "
                     "scale or the resolution and it runs again.",
                     workWidth, workHeight, (uint32_t) result, NgxResultName((unsigned int) result));
        }

        g_nr.rejectedWork = true;
        g_nr.rejectedWorkWidth = workWidth;
        g_nr.rejectedWorkHeight = workHeight;
    }
    else if (!NoteModelFault())
    {
        g_nr.failed = true;
        g_nr.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR evaluate returned 0x{:X} ({}), disabling for this session", (uint32_t) result,
                  NgxResultName((unsigned int) result));
    }

    // "Measure detail": in-game detail and temporal stability metrics across 60 evaluations on a still scene.
    // The session's flags are written by the menu under its mutex; it is taken once here, after g_nrMutex, the
    // same order every path uses.
    {
        std::lock_guard<std::mutex> lock(g_detailMeasure.mutex);

        const bool reshaped =
            g_detailMeasure.running && (g_detailMeasure.width != width || g_detailMeasure.height != height ||
                                        g_detailMeasure.format != desc.Format);

        if (g_detailMeasure.cancelRequested || reshaped)
        {
            // A cancel ends the run; a new size or format starts it again on the next evaluate.
            ParkDetailMeasureLocked();
            g_detailMeasure.wanted = g_detailMeasure.wanted && !g_detailMeasure.cancelRequested;
            g_detailMeasure.cancelRequested = false;
            g_detailMeasure.status.running = g_detailMeasure.wanted;
        }

        if (g_detailMeasure.wanted && wrote && DlssNr_DetailStats_Dx12::Available())
        {
            if (!g_detailMeasure.running)
            {
                g_detailMeasure.running = true;
                g_detailMeasure.width = width;
                g_detailMeasure.height = height;
                g_detailMeasure.format = desc.Format;
                g_detailMeasure.frozenWhitePoint = whitePoint;
                g_detailMeasure.attempts = 0;
                g_detailMeasure.evaluatesWithoutSample = 0;
                g_detailMeasure.currentCopy = 0;
                g_detailMeasure.hasPreviousFrame = false;
                g_detailMeasure.slot = 0;
                for (bool& p : g_detailMeasure.slotPending)
                    p = false;
                g_detailMeasure.accumulator.Reset();
                g_detailMeasure.status.samples = 0;
                g_detailMeasure.status.progress = 0.0f;
                g_detailMeasure.status.endReason = nullptr;
                if (g_detailMeasure.pass == nullptr)
                    g_detailMeasure.pass = new DlssNr_DetailStats_Dx12("DLSS-NR detail stats", device);

                if (g_detailMeasure.pass != nullptr && !g_detailMeasure.pass->IsInit())
                {
                    LOG_ERROR("DLSS-NR measure detail: compute pass initialization failed");
                    g_detailMeasure.status.initFailed = true;
                    g_detailMeasure.status.running = false;
                    g_detailMeasure.running = false;
                    g_detailMeasure.wanted = false;
                    ParkNrDetailStats(g_detailMeasure.pass);
                }
            }

            if (g_detailMeasure.pass != nullptr && g_detailMeasure.pass->IsInit())
            {
                // Collect the readbacks whose submission completed. Every one counts as an attempt; only still,
                // finite frames are kept by the accumulator, and only those count towards the target.
                for (uint32_t s = 0; s < DLSSNR_DETAIL_STATS_SLOTS; ++s)
                {
                    if (g_detailMeasure.slotPending[s] && DlssNr::Submission::Completed(g_detailMeasure.slotUsage[s]))
                    {
                        DlssNr::DetailStats::Stats st {};
                        if (g_detailMeasure.pass->Readback(s, st))
                        {
                            ++g_detailMeasure.attempts;
                            g_detailMeasure.accumulator.Add(st);
                            g_detailMeasure.evaluatesWithoutSample = 0;
                        }
                        g_detailMeasure.slotPending[s] = false;
                    }
                }

                const auto kept = static_cast<unsigned>(g_detailMeasure.accumulator.Count());
                g_detailMeasure.status.samples = kept;
                g_detailMeasure.status.progress = (float) kept / (float) DetailMeasureSession::kTarget;

                const bool enough = kept >= DetailMeasureSession::kTarget;
                const bool exhausted = g_detailMeasure.attempts >= DetailMeasureSession::kMaxAttempts;
                const char* stalled = nullptr;

                if (!enough && !exhausted)
                {
                    const unsigned cur = g_detailMeasure.currentCopy;
                    const unsigned prev = cur ^ 1;

                    // Copy target (UNORDERED_ACCESS) and hdrCopy (NON_PIXEL_SHADER_RESOURCE) into our owned copies
                    DetailMeasureCopy(cmdList, device, g_detailMeasure.outputs[cur],
                                      g_detailMeasure.outputReadable[cur], target,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    DetailMeasureCopy(cmdList, device, g_detailMeasure.inputs[cur], g_detailMeasure.inputReadable[cur],
                                      g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                    // The slot's constants, descriptors and readback are only rewritten once its last sample was
                    // collected; a slot still in flight skips this evaluate. Track before recording any use.
                    const uint32_t currentSlot = g_detailMeasure.slot;
                    if (g_detailMeasure.hasPreviousFrame && !g_detailMeasure.slotPending[currentSlot])
                    {
                        if (!DlssNr::Submission::Track(cmdList, g_detailMeasure.slotUsage[currentSlot]))
                        {
                            LOG_INFO("DLSS-NR measure detail: the command list cannot be tracked ({}); measurement "
                                     "stopped",
                                     DlssNr::Submission::LastRefusal());
                            stalled = "Measure detail stopped: this command list cannot be tracked (see the log).";
                        }
                        else if (g_detailMeasure.pass->Record(
                                     cmdList, g_detailMeasure.outputs[cur], g_detailMeasure.outputs[prev],
                                     g_detailMeasure.inputs[cur], g_detailMeasure.inputs[prev], modelInput, width,
                                     height, g_detailMeasure.frozenWhitePoint, currentSlot))
                        {
                            g_detailMeasure.slotPending[currentSlot] = true;
                            g_detailMeasure.slot = (currentSlot + 1) % DLSSNR_DETAIL_STATS_SLOTS;
                        }
                    }

                    g_detailMeasure.currentCopy ^= 1;
                    g_detailMeasure.hasPreviousFrame = true;

                    if (stalled == nullptr &&
                        ++g_detailMeasure.evaluatesWithoutSample > DetailMeasureSession::kStallEvaluates)
                        stalled = "Measure detail stopped: no sample completed (see the log).";
                }

                if (enough || exhausted || stalled != nullptr)
                {
                    if (stalled == nullptr && kept >= DetailMeasureSession::kMinKept)
                    {
                        const auto finalMeasurement = g_detailMeasure.accumulator.Finish(width, height);
                        if (g_detailMeasure.status.latest.samples > 0)
                        {
                            g_detailMeasure.status.previous = g_detailMeasure.status.latest;
                            g_detailMeasure.status.hasPrevious = true;
                        }
                        g_detailMeasure.status.latest = finalMeasurement;
                        g_detailMeasure.status.detailWords = finalMeasurement.DetailWords();
                        g_detailMeasure.status.flickerWords = finalMeasurement.FlickerWords();
                        g_detailMeasure.status.colourWords = finalMeasurement.ColourWords();
                        g_detailMeasure.status.shadowWords = finalMeasurement.ShadowWords();
                        g_detailMeasure.status.compareWords = finalMeasurement.Compare(g_detailMeasure.status.previous);

                        LOG_INFO("DLSS-NR measure detail ({} still frames of {} read): {} | {} | {} | {}", kept,
                                 g_detailMeasure.attempts, g_detailMeasure.status.detailWords,
                                 g_detailMeasure.status.flickerWords, g_detailMeasure.status.colourWords,
                                 g_detailMeasure.status.shadowWords);
                    }
                    else
                    {
                        // The last result stays on screen; this run produced none.
                        g_detailMeasure.status.endReason =
                            stalled != nullptr
                                ? stalled
                                : "Measure detail: the scene moved, too few still frames (turn on Hold frame).";
                        LOG_INFO("DLSS-NR measure detail: no result, {} still frames of {} read ({})", kept,
                                 g_detailMeasure.attempts, g_detailMeasure.status.endReason);
                    }

                    ParkDetailMeasureLocked();
                    g_detailMeasure.wanted = false;
                    g_detailMeasure.status.running = false;
                }
            }
        }
    }

    // The pulse metric ([DlssNr] PulseMetric, docs/dlssnr/design/pulse-metric.md): the edit's light on every frame the
    // pass wrote, its change from the frame before filed under the kind of frame this one was. Off builds and records
    // nothing; on, one small reduction of the target against the untouched copy, after the stabilizer, so it measures
    // what is shown. The same reduction is the exposure scan's meter when its match wants one ([DlssNr] ScanMatch);
    // with both off it builds and records nothing.
    UpdatePulse(cmdList, device, cfg.DlssNrPulseMetric.value_or_default(), DlssNr::ExposureScan::MeterWanted(), target,
                g_nr.hdrCopy, wrote, width, height, static_cast<uint32_t>(desc.Format), cadenceInputs.requested,
                cadenceInputs.smoothing, DlssNr::ResidualLimitStops(cfg.DlssNrResidualLimit.value_or_default()),
                DlssNr::MinimumRatio(cfg.DlssNrMinRatio.value_or_default()), cadence.cadence, carried, observedFrame,
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());

    hdrRead.Restore();

    depthCloneRead.Restore();
    motionCloneRead.Restore();

    smallRead.Restore();

    // Leave the staging copy as the next frame expects to find it.
    colorRead.Restore();

    // What the cadence learns from this frame: a stored edit restarts its count, a carried frame advances it,
    // and anything else -- a failed or partial model frame, a failed carry -- leaves nothing usable to carry.
    if (carried && result == 1)
        g_cadence.Carried(cadence.limit && g_nr.cadence != nullptr && g_nr.cadence->Smoothing());
    else if (!carried && cadenceRecorded)
        g_cadence.Recorded(cadenceFaded);
    else
        g_cadence.Drop(carried, !carried && result == 1);

    // Arm, but do not record the final timestamp yet. All remaining resource/state destructors
    // precede timing's destructor. Partial chains and failed composition never publish success.
    // A carried frame calls no model by design: zero passes is its whole chain, not a partial one, and its
    // sample says it was carried (GpuTiming::Metadata::carried).
    if (!carried && chain.completed != passSnapshot.Count)
        timing.Reject("partial-chain");
    const bool timingComplete =
        carried ? timingMetadataReady && wrote && result == 1
                : timingMetadataReady && wrote && result == 1 && chain.completed == passSnapshot.Count;
    timing.FinishOnScopeExit(carried ? 0u : chain.completed, timingComplete);
    DlssNr::Replay::NoteDispatch(wrote, carried, cadence.cadence, passSnapshot.Count, carried ? 0u : chain.completed,
                                 result);

    // restoreOutput hands the output back in its arrival state on every exit.
    device->Release();
    return wrote;
}

namespace DlssNr
{
void RetryAfterFailure()
{
    std::lock_guard<std::mutex> preLock(g_preMutex);
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    g_preAllocationFailed = false;
    g_preExtent.Reset();
    g_postExtent.Reset();
    g_nr.failed = false;
    for (auto& failed : g_nr.passFailed)
        failed = false;
    g_nr.peripheryFailed = false;
    g_nr.reason = "";
    g_nr.reset = true;

    // Transfer 2 starts over too: its private DLSS is rebuilt from nothing on the next frame that wants it.
    ParkNrEnlarger(g_enlarge.live);
    g_enlarge.failed = false;
}

void RetryEnlargement()
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    ParkNrEnlarger(g_enlarge.live);
    g_enlarge.failed = false;
}

Enlarge::Status EnlargeStatus()
{
    Enlarge::Status status;
    status.transfer = g_enlarge.transfer.load();
    status.why = static_cast<Enlarge::Why>(g_enlarge.why.load());
    return status;
}

// Internal type: PresentGuideClone (DlssNr_Dx12_Internal.h)

PresentGuideClone Dx12Internal::g_presentGuideClones[DlssNr::PresentGuides::kRingSize];
// The bridge's last captured-guides read, until its recording executes or is abandoned.
// Internal type: BridgeGuideRead (DlssNr_Dx12_Internal.h)
BridgeGuideRead Dx12Internal::g_bridgeRead;
DlssNr::PresentGuides::Ring Dx12Internal::g_presentGuideRing;

std::atomic<bool> Dx12Internal::g_temporalValid { false };

// When the D3D12 present hook last ran, for Auto. Auto hands the pass to the present hook once the
// upscaler has handed over its guides -- but only a D3D12 swapchain has that hook. The D3D11 bridge
// (wrapped_swapchain skips it) and every Vulkan route never reach RunPresentPass, so deferring there
// ran nothing at all: Auto was a silent off switch on the bridges. Auto now resolves to present only
// while the hook is demonstrably running.
std::atomic<long long> Dx12Internal::g_presentHookAtMs { 0 };

long long Dx12Internal::NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static bool PresentHookLive()
{
    const long long at = g_presentHookAtMs.load();
    return at != 0 && NowMs() - at < 500;
}

// The pass at the game's own Present, ahead of its DLSS-G (RunPresentPass's boundary entry), ran within the last half
// second. While it is live the entries below it stand down.
std::atomic<long long> Dx12Internal::g_boundaryAtMs { 0 };

// A present entry chose to run the pass (past the placement and the interim refusal) within the last half second: the
// present route is the one in use, whether or not its guides have been decided on yet. For HookStatus.
std::atomic<long long> Dx12Internal::g_presentRouteAtMs { 0 };
static bool PresentRouteLive()
{
    const long long at = g_presentRouteAtMs.load();
    return at != 0 && NowMs() - at < 500;
}
static bool BoundaryLive()
{
    const long long at = g_boundaryAtMs.load();
    return at != 0 && NowMs() - at < 500;
}

// What the present pass needs to know about the game's own DLSS-G (DlssNr_PresentBoundary.h). Notes it for Auto as a
// side effect, and says so once.
static DlssNr::PresentBoundary::Inputs BoundaryInputs(uint32_t hookMethod, bool fgHook, bool boundary = false)
{
    auto& state = State::Instance();

    DlssNr::PresentBoundary::Inputs in;
    in.hookMethod = hookMethod;
    in.fgHook = fgHook;
    in.ownFgActive = state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused();
    in.gameDlssgSet = state.dlssgLastSetMode != sl::DLSSGMode::eOff;
    in.gameDlssgCount = state.dlssgDetectedInterpolationCount;
    in.boundaryLive = boundary || BoundaryLive();

    if (DlssNr::PresentBoundary::NoteGameDlssg(in) && hookMethod == 0)
        LOG_INFO("DLSS-NR: the game's own DLSS-G is active, so Auto keeps the pass on the upscaler's output for this "
                 "session: there DLSS-G interpolates frames that already carry the edit, while a present pass would "
                 "edit one present of each cycle (nr-before-dlssg.md)");

    return in;
}
unsigned long long Dx12Internal::g_renderSeq = 0;
unsigned long long Dx12Internal::g_nrLastEnhancedSeq = 0;
unsigned long long Dx12Internal::g_nrLastEnhancedRecordedSeq = 0;
unsigned long long Dx12Internal::g_presentRuns = 0;
unsigned long long Dx12Internal::g_presentSkips = 0;
ID3D12Resource* Dx12Internal::g_lastEnhancedBb = nullptr;
unsigned int Dx12Internal::g_lastEnhancedBbIdx = 0xFFFFFFFF;
unsigned int Dx12Internal::g_sameBbStreak = 0, Dx12Internal::g_maxSameBbStreak = 0;

// Present-guide identity, step 1 of docs/dlssnr/design/present-real-guides.md: the stamp each capture carries, the
// rules every present's pick goes through before the pass may read it, the record of what each present did, the
// reset a lasting change of guide source owes the model, and the status the menu and the status line read. Nothing
// here runs at HookMethod=1, and with a ring that never recorded (a title with no upscaler) nothing but the status.
// Guarded by g_nrMutex, which every user already holds: the capture, both consumers' presents, and shutdown. The
// status goes out through atomics and is read without it.
// Internal type: PresentGuideIdentityState (DlssNr_Dx12_Internal.h)
PresentGuideIdentityState Dx12Internal::g_guideIdentity;

// The last second of guide outcomes, from whichever present host ran, and when it was published; and what that host
// did with its last frame that had no guides of the game's (PresentGuideIdentity::Fallback). For HookStatus and the
// menu.
std::atomic<uint64_t> Dx12Internal::g_guideStatus { 0 };
std::atomic<long long> Dx12Internal::g_guideStatusAtMs { 0 };
std::atomic<uint8_t> Dx12Internal::g_guideFallback { static_cast<uint8_t>(
    DlssNr::PresentGuideIdentity::Fallback::Zero) };

// The size of the frame the D3D11 bridge's present host last ran on, packed width << 32 | height; zero before its
// first. The native D3D11 guide transport leaves out an evaluate whose output is another size (DlssNr_Dx11Guides.h).
std::atomic<uint64_t> Dx12Internal::g_bridgePresentedExtent { 0 };

static void ReleasePresentTemporal()
{
    g_bridgeRead = {};
    for (auto& clone : g_presentGuideClones)
    {
        if (clone.depth != nullptr)
        {
            clone.depth->Release();
            clone.depth = nullptr;
        }
        if (clone.motion != nullptr)
        {
            clone.motion->Release();
            clone.motion = nullptr;
        }
        clone = PresentGuideClone {};
    }
    g_presentGuideRing.Reset();
    g_guideIdentity = {};
    g_temporalValid = false;
}

// The pass object, built once on the device the first frame arrives on. Four routes reach for it and
// two of them run on the present thread, so the build holds the lock: two threads each building one
// would leak the first and hand a caller an object the other has replaced.
static bool EnsureCompose(ID3D12Device* device)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    if (!g_nrDeviceOwner.Accept(device))
        return false;
    if (g_compose == nullptr)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", device);
    return g_compose != nullptr;
}

// Internal type: PresentList (DlssNr_Dx12_Internal.h)

PresentList Dx12Internal::g_presentList;
UINT64 Dx12Internal::g_presentFenceValue = 0;
unsigned long long Dx12Internal::g_presentFlip = 0;
unsigned long long Dx12Internal::g_lastFgFlip = 0;
static constexpr unsigned long long kFgHookTimeout = 16;
// The upscaled route (HookMethod 1, and 0 until the upscaler hands over temporal inputs) never
// enters the present-time body, so it keeps its own render count and last outcome here.
// ReportRuntimeStatus reads both; that is what makes the status line route-agnostic.
unsigned long long Dx12Internal::g_upscaleRenders = 0;
// Written on the render thread, read on the present thread, so atomic. The last outcome alone is not
// the window's: an evaluate clears it before it dispatches, and a present that reads it in that gap
// reported "no pass this window" over windows where every frame ran (STALKER 2, Starfield, Cyberpunk).
// The window's answer is g_passesRecorded, which only ever counts up; the flag stays for the reason.
std::atomic<bool> Dx12Internal::g_upscaleApplied { false };
std::atomic<const char*> Dx12Internal::g_upscaleReason { nullptr };
std::atomic<unsigned long long> Dx12Internal::g_passesRecorded { 0 };
// Which route the model's history belongs to. Switching routes owes a reset, requested through
// g_resetOnReturn because neither route holds the lock at the point it notices.
std::atomic<bool> Dx12Internal::g_sourceIsPresent { false };
ID3D12Resource* Dx12Internal::g_bbCopy = nullptr;
unsigned int Dx12Internal::g_bbCopyWidth = 0;
unsigned int Dx12Internal::g_bbCopyHeight = 0;
DXGI_FORMAT Dx12Internal::g_bbCopyFormat = DXGI_FORMAT_UNKNOWN;

// The D3D12 present route's synthesized motion ([DlssNr] SynthMotion). One, like the backbuffer copy
// it reads: this route runs one swapchain's presents. Heap-allocated on first use and never destroyed
// by a static destructor, which would release D3D12 objects at process exit after the device; Shutdown
// releases what it holds once the present lists have finished.
DlssNr::SynthMotionGuide* Dx12Internal::g_presentMotion = nullptr;

static void ResetPresentList()
{
    for (unsigned int i = 0; i < PresentList::kSlots; ++i)
    {
        if (g_presentList.list[i] != nullptr)
        {
            g_presentList.list[i]->Release();
            g_presentList.list[i] = nullptr;
        }

        if (g_presentList.allocator[i] != nullptr)
        {
            g_presentList.allocator[i]->Release();
            g_presentList.allocator[i] = nullptr;
        }

        g_presentList.fenceValue[i] = 0;
        g_presentList.dirty[i] = false;
    }

    if (g_presentList.fence != nullptr)
    {
        g_presentList.fence->Release();
        g_presentList.fence = nullptr;
    }

    if (g_presentList.fenceEvent != nullptr)
    {
        CloseHandle(g_presentList.fenceEvent);
        g_presentList.fenceEvent = nullptr;
    }

    g_presentFenceValue = 0;
}

static bool EnsurePresentList(ID3D12Device* device)
{
    if (g_presentList.list[0] != nullptr)
        return true;

    for (unsigned int i = 0; i < PresentList::kSlots; ++i)
    {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&g_presentList.allocator[i]))))
        {
            ResetPresentList();
            return false;
        }

        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_presentList.allocator[i], nullptr,
                                             IID_PPV_ARGS(&g_presentList.list[i]))))
        {
            ResetPresentList();
            return false;
        }

        // Named so a DRED report (DlssNr_Dred.h) attributes a removal in this list to the pass directly.
        const std::wstring name = std::format(L"DLSS-NR present list {}", i);
        g_presentList.allocator[i]->SetName(name.c_str());
        g_presentList.list[i]->SetName(name.c_str());
        g_presentList.list[i]->Close();
    }

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_presentList.fence))))
    {
        ResetPresentList();
        return false;
    }

    g_presentList.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (g_presentList.fenceEvent == nullptr)
    {
        ResetPresentList();
        return false;
    }

    return true;
}

static ID3D12GraphicsCommandList* GetPresentCommandList(ID3D12Device* device, unsigned int slot)
{
    if (g_presentList.list[0] == nullptr && !EnsurePresentList(device))
        return nullptr;

    if (!g_presentList.dirty[slot])
    {
        if (g_presentList.fenceValue[slot] != 0 &&
            g_presentList.fence->GetCompletedValue() < g_presentList.fenceValue[slot])
        {
            if (SUCCEEDED(g_presentList.fence->SetEventOnCompletion(g_presentList.fenceValue[slot],
                                                                    g_presentList.fenceEvent)))
            {
                const DWORD wait = WaitForSingleObject(g_presentList.fenceEvent, 5000);
                if (wait != WAIT_OBJECT_0)
                    LOG_WARN("DLSS-NR present: allocator wait returned {} (slot {})", (unsigned) wait, slot);
            }

            if (g_presentList.fence->GetCompletedValue() < g_presentList.fenceValue[slot])
            {
                static bool s_loggedBusy[PresentList::kSlots] = {};
                if (!s_loggedBusy[slot])
                {
                    s_loggedBusy[slot] = true;
                    LOG_WARN("DLSS-NR present: allocator slot {} still in use by GPU; skipping present pass", slot);
                }
                return nullptr;
            }
        }

        HRESULT hr = g_presentList.allocator[slot]->Reset();
        if (FAILED(hr))
        {
            LOG_WARN("DLSS-NR present: allocator reset failed on slot {} (HRESULT {:08X})", slot, (unsigned) hr);
            return nullptr;
        }

        hr = g_presentList.list[slot]->Reset(g_presentList.allocator[slot], nullptr);
        if (FAILED(hr))
        {
            LOG_WARN("DLSS-NR present: list reset failed on slot {} (HRESULT {:08X})", slot, (unsigned) hr);
            return nullptr;
        }

        g_presentList.dirty[slot] = true;
    }

    return g_presentList.list[slot];
}

// Whether the list was executed, which is the answer anything recorded on it is owed.
static bool SubmitPresentList(ID3D12CommandQueue* queue, unsigned int slot)
{
    if (!g_presentList.dirty[slot])
        return false;

    bool executed = false;

    if (SUCCEEDED(g_presentList.list[slot]->Close()))
    {
        queue->ExecuteCommandLists(1, (ID3D12CommandList**) &g_presentList.list[slot]);
        g_presentList.fenceValue[slot] = ++g_presentFenceValue;
        queue->Signal(g_presentList.fence, g_presentList.fenceValue[slot]);
        executed = true;
    }

    g_presentList.dirty[slot] = false;
    return executed;
}

// Every present list submitted so far has finished on the GPU. Bounded; false on a timeout, in which
// case whatever those lists read is kept, not released.
static bool WaitPresentListsIdle(DWORD timeoutMs)
{
    if (g_presentList.fence == nullptr || g_presentFenceValue == 0 ||
        g_presentList.fence->GetCompletedValue() >= g_presentFenceValue)
    {
        return true;
    }

    if (FAILED(g_presentList.fence->SetEventOnCompletion(g_presentFenceValue, g_presentList.fenceEvent)))
        return false;

    return WaitForSingleObject(g_presentList.fenceEvent, timeoutMs) == WAIT_OBJECT_0;
}

// The present route's synthesized motion is owed the same answer as the list it was recorded on.
static void SettlePresentMotion(bool executed)
{
    if (g_presentMotion == nullptr)
        return;

    if (executed)
        g_presentMotion->ConfirmExecuted();
    else
        g_presentMotion->AbandonRecording();
}

static void DropPresentList(unsigned int slot)
{
    if (!g_presentList.dirty[slot])
        return;

    if (FAILED(g_presentList.list[slot]->Close()))
        LOG_WARN("DLSS-NR present: dropped list slot {} would not close", slot);

    // It will never execute: its submission epoch is abandoned, or it stays sealed and is never pruned.
    DlssNr::Submission::Abandon(g_presentList.list[slot]);
    g_presentList.dirty[slot] = false;
}

static bool EnsureBbCopy(ID3D12Device* device, unsigned int width, unsigned int height, DXGI_FORMAT format)
{
    if (g_bbCopy != nullptr && g_bbCopyWidth == width && g_bbCopyHeight == height && g_bbCopyFormat == format)
        return true;

    ParkNrResource(g_bbCopy);

    g_bbCopy = CreateScratch(device, format, width, height);
    g_bbCopyWidth = width;
    g_bbCopyHeight = height;
    g_bbCopyFormat = format;

    return g_bbCopy != nullptr;
}

// HDR10 and scRGB at present ([DlssNr] HdrPresent, docs/dlssnr/design/hdr-present-conversion.md section 5.1). The
// conversion pass, built once for the device this route runs on, and its two textures, made only once a frame is
// decided HDR -- an SDR game never allocates them -- and retired when the size or format changes, or when the frames
// go back to SDR.
//
// Retired by the present lists' own fence, not parked on the NR recording's usage like the backbuffer copy: a list
// whose pass declined still decoded into the work texture, and a switch back to SDR comes with no resize and so no GPU
// idle wait. Every list that used a texture was submitted before it was retired, so it is released once the fence has
// passed the last value submitted then -- never under the GPU, and without a CPU wait.
DlssNr_PresentHdr_Dx12* Dx12Internal::g_presentHdr = nullptr;
// Internal type: PresentHdrRetired (DlssNr_Dx12_Internal.h)
std::vector<PresentHdrRetired> Dx12Internal::g_hdrRetired;
ID3D12Resource* Dx12Internal::g_hdrWork = nullptr; // FP16, linear Rec.709 in scRGB units; rests in UNORDERED_ACCESS
ID3D12Resource* Dx12Internal::g_hdrOut =
    nullptr; // the backbuffer's format, what the encode writes; rests in UNORDERED_ACCESS
unsigned int Dx12Internal::g_hdrWidth = 0;
unsigned int Dx12Internal::g_hdrHeight = 0;
DXGI_FORMAT Dx12Internal::g_hdrFormat = DXGI_FORMAT_UNKNOWN;
// The size and format the textures last failed to allocate at: not retried every frame.
unsigned int Dx12Internal::g_hdrFailedWidth = 0;
unsigned int Dx12Internal::g_hdrFailedHeight = 0;
DXGI_FORMAT Dx12Internal::g_hdrFailedFormat = DXGI_FORMAT_UNKNOWN;

// Under g_nrMutex. Released once every present list submitted so far has completed (see above).
static void RetirePresentHdrTexture(ID3D12Resource*& resource)
{
    if (resource == nullptr)
        return;

    g_hdrRetired.push_back({ resource, g_presentFenceValue });
    resource = nullptr;
}

// Under g_nrMutex, once per present: the retired textures the GPU is done with. Nothing pending, nothing done.
static void CollectPresentHdrRetired()
{
    if (g_hdrRetired.empty() || g_presentList.fence == nullptr)
        return;

    const UINT64 completed = g_presentList.fence->GetCompletedValue();
    for (size_t i = 0; i < g_hdrRetired.size();)
    {
        if (g_hdrRetired[i].fenceValue <= completed)
        {
            g_hdrRetired[i].resource->Release();
            g_hdrRetired[i] = g_hdrRetired.back();
            g_hdrRetired.pop_back();
        }
        else
        {
            ++i;
        }
    }
}

// Under g_nrMutex. An SDR frame does not keep the textures a game that left HDR no longer uses.
static void ParkPresentHdrTextures()
{
    if (g_hdrWork == nullptr && g_hdrOut == nullptr)
        return;

    RetirePresentHdrTexture(g_hdrWork);
    RetirePresentHdrTexture(g_hdrOut);
    g_hdrWidth = 0;
    g_hdrHeight = 0;
    g_hdrFormat = DXGI_FORMAT_UNKNOWN;
}

// Under g_nrMutex. nullptr once the pass and both textures are ready for this frame; otherwise why not, a literal, and
// the frame is declined the way an HDR swapchain always was.
static const char* EnsurePresentHdr(ID3D12Device* device, unsigned int width, unsigned int height, DXGI_FORMAT format)
{
    if (!DlssNr_PresentHdr_Dx12::Available())
        return PresentColourText::kNoShader;

    if (g_presentHdr == nullptr)
        g_presentHdr = new DlssNr_PresentHdr_Dx12("DLSS-NR present HDR", device);

    if (!g_presentHdr->BuiltFor(device))
        return PresentColourText::kOtherDevice;

    if (!g_presentHdr->IsInit())
        return PresentColourText::kNotBuilt;

    // Typed UAV store into the swapchain's format and into FP16, asked of the device, not assumed (section 5.4).
    if (!g_presentHdr->Supports(format))
        return PresentColourText::kNoTypedStore;

    if (g_hdrWork != nullptr && g_hdrOut != nullptr && g_hdrWidth == width && g_hdrHeight == height &&
        g_hdrFormat == format)
    {
        return nullptr;
    }

    if (g_hdrFailedWidth == width && g_hdrFailedHeight == height && g_hdrFailedFormat == format)
        return PresentColourText::kNoScratch;

    // Retired, not released: the present lists that used them may still be on the GPU.
    ParkPresentHdrTextures();
    g_hdrWork = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
    g_hdrOut = CreateScratch(device, format, width, height);

    if (g_hdrWork == nullptr || g_hdrOut == nullptr)
    {
        RetirePresentHdrTexture(g_hdrWork);
        RetirePresentHdrTexture(g_hdrOut);
        g_hdrFailedWidth = width;
        g_hdrFailedHeight = height;
        g_hdrFailedFormat = format;
        LOG_WARN("DLSS-NR present: the HDR conversion's {}x{} textures could not be created", width, height);
        return PresentColourText::kNoScratch;
    }

    g_hdrWidth = width;
    g_hdrHeight = height;
    g_hdrFormat = format;
    LOG_INFO("DLSS-NR present: HDR conversion textures built for {}x{}, swapchain format {}", width, height,
             (int) format);
    return nullptr;
}

// A UAV barrier: the decode's writes to the work texture are done before the pass touches it, whatever it does first.
static void PresentHdrUavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    list->ResourceBarrier(1, &barrier);
}

// The colour decision for this present (DlssNr_PresentColour.h), and the menu told about it. With HdrPresent off this
// is PresentColourRefusal on the inputs it always had: the process-wide colour space, or PQ where ForceHDR and UseHDR10
// set it themselves without the wrapper seeing it. With it on, the colour space recorded for this swapchain decides.
static PresentColourDecision DecidePresentColour(IDXGISwapChain3* swapchain, DXGI_FORMAT format, const Config& cfg)
{
    const DXGI_COLOR_SPACE_TYPE legacy = cfg.ForceHDR.value_or_default() && cfg.UseHDR10.value_or_default()
                                             ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                             : State::Instance().outputColorSpace.dxgiColorSpace;
    const bool hdrPresent = cfg.DlssNrHdrPresent.value_or_default();
    const PresentColourRecord record = SwapchainColour::Lookup(swapchain);
    const PresentColourDecision switchOn = ResolvePresentColour(format, record, true, legacy);

    if (ReportsHdrStatus(format, switchOn, hdrPresent))
        NotePresentHdr(switchOn.profile, hdrPresent ? switchOn.reason : nullptr, false);

    return hdrPresent ? switchOn : ResolvePresentColour(format, record, false, legacy);
}

// One line, on a cadence, that answers the questions a session actually asks: is the pass running,
// how many frames reach the screen for each one rendered, and why not when it is not.
//
// Written because those answers were in the log all along -- as a frametime here, a present counter
// there, an NGX code three hundred lines up -- and reconstructing them by hand cost real time. The
// present:render ratio is the multiplier that actually left the GPU, which is a different thing from
// the multiplier that was requested; a request of 4x that pace-fails to 1x looks identical in config
// and opposite on screen. render frames are counted at CaptureTemporal (g_renderSeq); present frames
// at the present hook (g_presentFlip).
void ReportRuntimeStatus(bool passRecorded, const char* skipReason)
{
    static unsigned long long lastPresent = 0;
    static unsigned long long lastRender = 0;
    static unsigned long long lastPasses = 0;
    static std::chrono::steady_clock::time_point lastAt {};

    const auto now = std::chrono::steady_clock::now();
    if (lastAt.time_since_epoch().count() != 0 && now - lastAt < std::chrono::seconds(2))
        return;

    const auto renderCount = g_renderSeq + g_upscaleRenders;
    const auto presents = g_presentFlip - lastPresent;
    const auto renders = renderCount - lastRender;
    const auto passCount = g_passesRecorded.load();
    const bool anyPass = passCount != lastPasses;
    lastPresent = g_presentFlip;
    lastRender = renderCount;
    lastPasses = passCount;
    lastAt = now;

    if (renders == 0 && presents == 0)
        return;

    const double ratio = renders != 0 ? (double) presents / (double) renders : 0.0;

    if (passRecorded || anyPass)
        LOG_INFO("DLSS-NR status: {} | {} presents : {} renders = {:.2f}x on screen", HookStatus(), presents, renders,
                 ratio);
    else
        LOG_INFO("DLSS-NR status: no pass this window ({}) | {} presents : {} renders", skipReason ? skipReason : "",
                 presents, renders);
}

const char* HookStatus()
{
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
        return "off";

    const uint32_t method = Config::Instance()->DlssNrHookMethod.value_or_default();

    // The guides a present route runs on come from what its presents did in the last second (present-real-guides.md,
    // D3), not from g_temporalValid, which latches on the first capture and kept saying "with DLSS temporal inputs"
    // after the upscaler had gone quiet. Routing still reads the latch; this does not. Which route is in use is the
    // present entry's own word (PresentRouteLive), so a present route that has not decided on guides yet -- its first
    // second, or frames it declines before its pick -- is still named as present.
    DlssNr::PresentGuideIdentity::Status guides;
    const bool presenting = RecentGuideStatus(&guides);

    if (method == 2)
    {
        if (BoundaryLive())
            return "present: at the game's own Present, ahead of its frame generation";
        if (DlssNr::PresentBoundary::g_lastDeclined.load())
            return "present: declined, the game's DLSS-G interpolates before this pass (HookMethod=1 edits the frames "
                   "it interpolates)";
        if (presenting)
            return DlssNr::PresentGuideIdentity::RouteText(false, RecentGuideSource(guides));
        if (PresentRouteLive())
            return "present: active on the swapchain's backbuffer";
        return "present: no present has reached the pass yet";
    }

    if (method == 0)
    {
        if (DlssNr::PresentBoundary::g_gameDlssgSeen.load())
            return "auto: upscaled, the game's DLSS-G would interpolate before a present pass";
        if (presenting && PresentRouteLive())
            return DlssNr::PresentGuideIdentity::RouteText(true, RecentGuideSource(guides));
        if (PresentRouteLive())
            return "auto: present, on the swapchain's backbuffer";
        return "auto: upscaled, the upscaler has handed over no temporal inputs yet";
    }

    return "upscaled: active on the upscaler's output";
}

void RunPresentPass(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, bool fgHook, bool boundary)
{
    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default())
    {
        g_resetOnReturn = true;
        DlssNr::SessionGuardOff();
        return;
    }

    if (!boundary)
        DlssNr::SlBoundaryProbe::NoteWrappedPresent(queue, fgHook);

    if (g_nr.failed || swapchain == nullptr || queue == nullptr)
        return;

    // Synthesized frame generation: the pass belongs to the base frame, once, before FG interpolates --
    // in the bridge's own host on the D3D11 bridge, and in the FG present hook (fgHook) on a native D3D12
    // swapchain. What reaches this entry instead is a wrapped swapchain of FFX's, and FFX presents every
    // frame, generated ones included; running here too put the model on each of them, after FG
    // (synthesized-frame-generation.md, step 1 measured). On native D3D12 the g_lastFgFlip stand-down
    // below would catch this only once the FG hook had recorded a pass; this holds from the first flip.
    // Nothing is counted or noted as live either, so the status line counts base frames. Only while an
    // FG swapchain exists: without one, this entry is the only route the pass has. A bridge or a
    // swapchain fed by an upscaler keeps the route it had.
    if (!fgHook && State::Instance().activeFgInput == FGInput::Synthesized &&
        (State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 ||
         State::Instance().currentFGSwapchain != nullptr))
    {
        return;
    }

    // The pass at the game's own Present owns every frame while it is live (nr-before-dlssg.md, step 1). The entries
    // below it -- the wrapped swapchain, which under the game's DLSS-G sees its generated presents too, and the FG
    // present hook -- stand down, uncounted, so the status line counts the game's presents at the boundary.
    if (boundary)
    {
        if (g_boundaryAtMs.exchange(NowMs()) == 0)
            LOG_INFO("DLSS-NR present: the pass runs at the game's own Present, ahead of its frame generation, on the "
                     "game's queue (nr-before-dlssg.md, step 1)");
    }
    else if (BoundaryLive())
    {
        return;
    }

    g_presentHookAtMs = NowMs();

    // Every present is counted once, whichever route owns the pass; it is the numerator of the
    // on-screen ratio the status line reports. With an OptiScaler-managed frame generator both the
    // FG present hook and the wrapped swapchain's hook reach here for the same flip, so the wrapped
    // entry does not count while it is standing down for the FG one -- the same rule that keeps it
    // from drawing twice below. (Read 2.00x on a 30 fps reprojection session before this check.)
    const bool wrapStandsDown =
        !fgHook && !boundary && g_lastFgFlip != 0 && (g_presentFlip - g_lastFgFlip) < kFgHookTimeout;

    if (!wrapStandsDown)
        ++g_presentFlip;

    const auto boundaryInputs = BoundaryInputs(cfg.DlssNrHookMethod.value_or_default(), fgHook, boundary);
    const auto verdict = DlssNr::PresentBoundary::Decide(boundaryInputs, g_temporalValid.load());

    if (verdict == DlssNr::PresentBoundary::PresentVerdict::Upscaled)
    {
        // The upscaled route recorded (or declined) its pass on the upscaler's command list; this
        // is still the one place per present to say so.
        DlssNr::PresentBoundary::g_lastDeclined = false;
        const char* upscaleReason = g_upscaleReason.load();
        ReportRuntimeStatus(g_upscaleApplied.load(), upscaleReason ? upscaleReason : "no upscaler evaluate yet");
        return;
    }

    if (wrapStandsDown)
        return;

    // HookMethod=2 under the game's own DLSS-G, on this entry below it: the frame is shown as the game drew it rather
    // than edited on one present of each cycle (DlssNr_PresentBoundary.h).
    if (verdict == DlssNr::PresentBoundary::PresentVerdict::Declined)
    {
        if (!DlssNr::PresentBoundary::g_lastDeclined.exchange(true))
            LOG_WARN("DLSS-NR: present pass declined: {}", DlssNr::PresentBoundary::kDeclinedReason);

        ReportRuntimeStatus(false, DlssNr::PresentBoundary::kDeclinedReason);
        return;
    }

    DlssNr::PresentBoundary::g_lastDeclined = false;
    g_presentRouteAtMs = NowMs();

    // Fail-open startup guard: defer neural composition during initial frames (first 4 flips)
    // so swapchains, window sizing, and DXGI/VKD3D presentation pipelines settle into a steady state.
    // Prevents black-screen deadlocks or 0 FPS hangs at boot under Proton/Wine.
    constexpr uint64_t kStartupWarmupFlips = 4;
    if (g_presentFlip <= kStartupWarmupFlips)
    {
        ReportRuntimeStatus(false, "startup deferred (stabilizing swapchain)");
        return;
    }

    // An HDR swapchain is declined before anything is built for it (DlssNr_PresentColour.h), unless HdrPresent is on
    // and it is HDR10 or scRGB, which is converted (docs/dlssnr/design/hdr-present-conversion.md). DecidePresentColour
    // has which colour space each answer is read from.
    PresentColour presentProfile = PresentColour::Sdr;
    DXGI_FORMAT presentFormat = DXGI_FORMAT_UNKNOWN;
    {
        DXGI_SWAP_CHAIN_DESC1 scDesc {};

        const HRESULT described = swapchain->GetDesc1(&scDesc);
        if (FAILED(described) && cfg.DlssNrHdrPresent.value_or_default())
        {
            g_resetOnReturn = true;
            if (fgHook)
                g_lastFgFlip = g_presentFlip;
            NotePresentHdr(PresentColour::Declined, PresentColourText::kNoDescription, false);
            ReportSkipOnce(PresentColourText::kNoDescription);
            ReportRuntimeStatus(false, PresentColourText::kNoDescription);
            return;
        }
        if (SUCCEEDED(described))
        {
            const PresentColourDecision decision = DecidePresentColour(swapchain, scDesc.Format, cfg);
            presentProfile = decision.profile;
            presentFormat = scDesc.Format;

            if (const char* refusal = decision.profile == PresentColour::Declined ? decision.reason : nullptr)
            {
                // What the model last saw is not the frame that follows once it runs again.
                g_resetOnReturn = true;

                // The FG hook still owns this flip, so the wrapped swapchain's entry keeps standing down
                // and the status line does not count every present twice.
                if (fgHook)
                    g_lastFgFlip = g_presentFlip;

                ReportSkipOnce(refusal);
                ReportRuntimeStatus(false, refusal);
                return;
            }
        }
    }

    ID3D12Resource* backbuffer = nullptr;
    const UINT index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer))) || backbuffer == nullptr)
    {
        ReportSkipOnce("the present hook could not reach the swapchain's backbuffer");
        return;
    }

    // The pass records on a list of its own and submits it to this queue, so the list is made on the queue's device.
    // Under a wrapper layer the backbuffer reports the device underneath (DlssNr_DeviceMatch.h); jlrouzies' 6724d677
    // submits on a queue of the device its lists were made on for the same reason.
    ID3D12Device* device = nullptr;

    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        backbuffer->Release();
        ReportSkipOnce("the present queue belongs to no D3D12 device");
        return;
    }

    const D3D12_RESOURCE_DESC bbDesc = backbuffer->GetDesc();
    if (IsHdrProfile(presentProfile) && !DlssNr_PresentHdr_Dx12::TextureShape(bbDesc))
    {
        backbuffer->Release();
        device->Release();
        g_resetOnReturn = true;
        if (fgHook)
            g_lastFgFlip = g_presentFlip;
        NotePresentHdr(presentProfile, PresentColourText::kResourceContract, false);
        ReportSkipOnce(PresentColourText::kResourceContract);
        ReportRuntimeStatus(false, PresentColourText::kResourceContract);
        return;
    }

    if (!DeviceChildBelongsTo(device, backbuffer) || !EnsureCompose(device))
    {
        backbuffer->Release();
        device->Release();
        ReportSkipOnce("the backbuffer or composition cache belongs to another device");
        return;
    }

    const unsigned int slot = index % PresentList::kSlots;
    ID3D12GraphicsCommandList* list = GetPresentCommandList(device, slot);

    if (list == nullptr)
    {
        backbuffer->Release();
        device->Release();
        ReportSkipOnce("the present hook's command list could not be prepared");
        return;
    }

    const unsigned int width = (unsigned int) bbDesc.Width;
    const unsigned int height = bbDesc.Height;
    const void* const presenter = PresentGuidePresenter(swapchain);

    // This is the present thread. Everything below that the render thread also touches -- the
    // backbuffer copy's retirement (the retired list Dispatch trims), the captured guides, the failure
    // latch -- is read or written under the lock, which is released again before the evaluate takes it.
    std::unique_lock<std::mutex> nrLock(g_nrMutex);

    if (!EnsureBbCopy(device, width, height, bbDesc.Format))
    {
        g_nr.failed = true;
        g_nr.reason = "the present hook's backbuffer copy could not be created";
        LOG_ERROR("DLSS-NR present: {}", g_nr.reason);
        nrLock.unlock();
        DropPresentList(slot);
        backbuffer->Release();
        device->Release();
        return;
    }

    // An HDR frame needs the conversion and its two textures; an SDR one parks them if a game just left HDR. A frame
    // the conversion cannot take after all is declined as an HDR swapchain always was: the game's own picture, said
    // once, and nothing else is spent on it. The decision read the swapchain's format; the copy is of this buffer.
    const bool presentHdr = IsHdrProfile(presentProfile);
    const char* hdrRefusal = nullptr;
    CollectPresentHdrRetired();
    if (presentHdr)
        hdrRefusal = bbDesc.Format != presentFormat ? PresentColourText::kBufferFormat
                                                    : EnsurePresentHdr(device, width, height, bbDesc.Format);
    else
        ParkPresentHdrTextures();
    if (presentHdr && hdrRefusal == nullptr)
    {
        const uint32_t profile =
            presentProfile == PresentColour::Hdr10 ? PresentHdr::kProfileHdr10 : PresentHdr::kProfileScRgb;
        if (!g_presentHdr->ValidResources(g_bbCopy, g_bbCopy, g_hdrWork, profile, PresentHdr::kModeDecode) ||
            !g_presentHdr->ValidResources(g_bbCopy, g_hdrWork, g_hdrOut, profile, PresentHdr::kModeEncode))
            hdrRefusal = PresentColourText::kResourceContract;
    }

    if (hdrRefusal != nullptr)
    {
        nrLock.unlock();
        DropPresentList(slot);
        backbuffer->Release();
        device->Release();
        g_resetOnReturn = true;
        if (fgHook)
            g_lastFgFlip = g_presentFlip;
        NotePresentHdr(presentProfile, hdrRefusal, false);
        ReportSkipOnce(hdrRefusal);
        ReportRuntimeStatus(false, hdrRefusal);
        return;
    }

    const auto presentWhite =
        presentHdr ? DlssNr::PresentHdr::DisplayWhiteForSwapchain(swapchain) : DlssNr::PresentHdr::WhiteLevel {};

    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    DlssNrFrameInfo frame {};
    bool temporalValid = false;
    unsigned long long temporalSeq = 0;

    static bool s_loggedSameQueue = false;
    static bool s_loggedCompleted = false;
    bool admittedSameQueue = false;
    bool admittedCompleted = false;

    // Pick from the owned guide snapshot ring: newest capture completed on CPU or submitted on same queue
    const auto nowMs = NowMs();
    const auto isEligible = [queue, &admittedSameQueue, &admittedCompleted](void* token) -> bool
    {
        if (token == nullptr)
            return false;
        auto& writeUsage = *static_cast<DlssNr::Submission::Usage*>(token);
        // RenoDX same-queue rule (present_path.hpp:96-100):
        // If write was submitted on the SAME queue as present, queue order guarantees completion.
        if (queue != nullptr && DlssNr::Submission::SubmittedOnlyOn(writeUsage, queue))
        {
            admittedSameQueue = true;
            return true;
        }
        // Cross-queue or unsubmitted: requires CPU completion proof
        if (DlssNr::Submission::Completed(writeUsage))
        {
            admittedCompleted = true;
            return true;
        }
        return false;
    };

    const size_t pickedSlot = g_presentGuideRing.PickForPresent(isEligible, nowMs);

    // The identity rules over that pick (present-real-guides.md, step 1). The pass reads the slot they return: the
    // pick, or none when a rule refused it, and then the frame falls back below exactly as with no capture. A refused
    // slot is never tracked or read. Rule 6 (lag) is enforced at the game's own Present only, where it was measured.
    const GuideAsk guides = AskPresentPassGuides(device, presenter, width, height, pickedSlot, boundary);
    const size_t usableSlot = guides.decision.slot;

    DlssNr::Submission::Usage savedReadUsage {};
    DlssNr::Submission::Usage savedPresentReadUsage {};
    bool savedReadTracked = false;
    size_t trackedSlot = DlssNr::PresentGuides::kRingSize;

    const auto newestRecordedSeq = g_presentGuideRing.NewestRecordedSequence();
    if (newestRecordedSeq > 0 && newestRecordedSeq == g_nrLastEnhancedRecordedSeq &&
        (!boundary || cfg.DlssNrRequireDlss.value_or_default()))
    {
        // Below the app boundary, no new capture can mean a generated frame: keep Correction 5's skip. At the
        // app's own Present, an upscaler can go quiet during video playback; when guides are optional, continue
        // through identity rejection and the existing zero/synthesized fallback instead (hellraiser-video-present.md).
        // Use this entry's boundary flag, not BoundaryLive(): lower entries must never inherit this permission.
        CountPresentPassGuides(guides, pickedSlot, DlssNr::PresentGuideIdentity::Outcome::Skipped);
        ++g_presentSkips;
        nrLock.unlock();
        DropPresentList(slot);
        backbuffer->Release();
        device->Release();
        return;
    }

    if (usableSlot < DlssNr::PresentGuides::kRingSize)
    {
        auto& clone = g_presentGuideClones[usableSlot];
        savedReadUsage = clone.readUsage;
        savedReadTracked = clone.readTracked;
        savedPresentReadUsage = clone.presentReadUsage;
        clone.presentReadUsage = {}; // reset for this specific present read (Correction 2)
        const bool tracked =
            DlssNr::Submission::Track(list, clone.readUsage) && DlssNr::Submission::Track(list, clone.presentReadUsage);
        if (tracked)
        {
            clone.readTracked = true;
            trackedSlot = usableSlot;
            depth = clone.depth;
            motion = clone.motion;
            frame = clone.frame;
            frame.DepthState = (int) D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            frame.MotionState = (int) D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            temporalValid = (depth != nullptr && motion != nullptr);
            temporalSeq = g_presentGuideRing.GetSlot(usableSlot).sequence;
        }
        else
        {
            // Track refused: restore saved usage and do NOT overwrite readTracked with false
            clone.readUsage = savedReadUsage;
            clone.presentReadUsage = savedPresentReadUsage;
            temporalValid = false;
        }
    }

    if (temporalValid)
    {
        g_nrLastEnhancedRecordedSeq = newestRecordedSeq;
        if (admittedSameQueue && !s_loggedSameQueue)
        {
            LOG_INFO("DLSS-NR present guides: guide snapshot admitted via same-queue execution order (zero CPU wait)");
            s_loggedSameQueue = true;
        }
        else if (admittedCompleted && !s_loggedCompleted)
        {
            LOG_INFO("DLSS-NR present guides: guide snapshot admitted via CPU completion proof (cross-queue)");
            s_loggedCompleted = true;
        }
    }

    const auto guideOutcome = DlssNr::PresentGuideIdentity::OutcomeOf(guides.decision, temporalValid);
    CountPresentPassGuides(guides, pickedSlot, guideOutcome);

    // The newest capture refused for good: decided on as surely as one used, so a later present with nothing newer
    // recorded skips above (D5), except at the app boundary with optional guides, where it runs the fallback.
    // A generated frame may reach a lower entry, never that boundary. A refused older pick (a newer
    // capture still being copied), or a refusal that turns on this present (SettlesNewest), is left to the next one.
    if (guideOutcome == DlssNr::PresentGuideIdentity::Outcome::Refused &&
        DlssNr::PresentGuideIdentity::SettlesNewest(guides.decision.reason) &&
        g_presentGuideRing.GetSlot(pickedSlot).sequence == newestRecordedSeq)
    {
        g_nrLastEnhancedRecordedSeq = newestRecordedSeq;
    }

    // The source the model's history is built on, for a pass that runs: under RequireDlss a frame without the game's
    // guides is skipped below and is not a source change. While the game's guides hold the history, a frame that
    // misses them takes no ZeroGuideReset either: that is a miss, not a scene without motion (D3).
    bool gameGuidesHold = false;
    if (temporalValid || !cfg.DlssNrRequireDlss.value_or_default())
        gameGuidesHold = NotePresentPassGuideSource(temporalValid, !g_sourceIsPresent.load());

    nrLock.unlock();

    static ZeroGuides s_dummyGuides;
    // The clear below is a one-way UNORDERED_ACCESS -> NON_PIXEL_SHADER_RESOURCE transition recorded once; the guides
    // believe it happened only when told, and a list dropped without telling them leaves every later frame handing
    // NGX guides it calls NON_PIXEL_SHADER_RESOURCE that are still in UNORDERED_ACCESS.
    bool dummyClearRecorded = false;
    const auto settleDummyGuides = [&](bool executed)
    {
        if (!dummyClearRecorded)
            return;
        if (executed)
            s_dummyGuides.ConfirmExecuted();
        else
            s_dummyGuides.AbandonRecording();
    };

    if (!temporalValid && cfg.DlssNrRequireDlss.value_or_default())
    {
        if (trackedSlot < DlssNr::PresentGuides::kRingSize)
        {
            std::lock_guard<std::mutex> lock(g_nrMutex);
            g_presentGuideClones[trackedSlot].readUsage = savedReadUsage;
            g_presentGuideClones[trackedSlot].presentReadUsage = savedPresentReadUsage;
            g_presentGuideClones[trackedSlot].readTracked = savedReadTracked;
        }
        DropPresentList(slot);
        backbuffer->Release();
        device->Release();
        NotePresentGuideWaiting();
        ReportSkipOnce("the present hook is waiting for the upscaler's temporal inputs");
        return;
    }
    else if (!temporalValid)
    {
        if (!s_dummyGuides.Ensure(device, width, height))
        {
            if (trackedSlot < DlssNr::PresentGuides::kRingSize)
            {
                std::lock_guard<std::mutex> lock(g_nrMutex);
                g_presentGuideClones[trackedSlot].readUsage = savedReadUsage;
                g_presentGuideClones[trackedSlot].presentReadUsage = savedPresentReadUsage;
                g_presentGuideClones[trackedSlot].readTracked = savedReadTracked;
            }
            DropPresentList(slot);
            backbuffer->Release();
            device->Release();
            std::lock_guard<std::mutex> failLock(g_nrMutex);
            g_nr.failed = true;
            g_nr.reason = "the present hook's dummy temporals could not be created";
            LOG_ERROR("DLSS-NR present: {}", g_nr.reason);
            return;
        }
        dummyClearRecorded = s_dummyGuides.RecordClear(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        depth = s_dummyGuides.Depth();
        motion = s_dummyGuides.Motion();
        frame = DlssNrFrameInfo {};
        frame.DepthState = (int) D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        frame.MotionState = (int) D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        // Zero guides above native are refused by the model (FAIL_InvalidParameter), so a working
        // scale above 1 is clamped to native here, as the D3D11 host does since fcc5fdf0. This route
        // reset the frame to its defaults, where supersampling is allowed, and so was left out.
        frame.AllowSupersampling = false;

        // See DlssNrZeroGuideReset: no real motion, so optionally no history to ghost from. Ours, not the
        // game's, and it says so.
        frame.Reset = cfg.DlssNrZeroGuideReset.value_or_default() && !gameGuidesHold;
        frame.ResetIsPolicy = frame.Reset;
    }

    ++g_presentRuns;
    g_sameBbStreak = (backbuffer == g_lastEnhancedBb) ? g_sameBbStreak + 1 : 0;
    g_maxSameBbStreak = g_sameBbStreak > g_maxSameBbStreak ? g_sameBbStreak : g_maxSameBbStreak;
    g_lastEnhancedBb = backbuffer;
    g_lastEnhancedBbIdx = index;

    if (!g_sourceIsPresent.exchange(true))
        g_resetOnReturn = true;

    const D3D12_RESOURCE_STATES bbState = D3D12_RESOURCE_STATE_PRESENT;

    Barrier(list, backbuffer, bbState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, g_bbCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(g_bbCopy, backbuffer);
    Barrier(list, g_bbCopy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(list, backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, bbState);

    // HDR (hdr-present-conversion.md section 4.2): from here the copy is the original codes, never written, and the
    // pass runs on its decode in the FP16 work texture. g_bbCopy rests in UNORDERED_ACCESS, where the motion estimator
    // below reads it, so it is a shader resource only around each dispatch. Both dispatches are timed as one
    // conversion sample (DlssNr_GpuTiming), which is finished before the list is closed or dropped.
    std::optional<DlssNr::GpuTiming::Conversion> hdrTiming;
    const uint32_t hdrShaderProfile =
        presentProfile == PresentColour::Hdr10 ? DlssNr::PresentHdr::kProfileHdr10 : DlssNr::PresentHdr::kProfileScRgb;
    bool hdrDecoded = false;
    if (presentHdr)
    {
        // Enabled exactly when the Dispatch's own sample is: under the driver proxy the menu says timing is off.
        const auto timingSettings = cfg.GetDlssNrGpuTimingSettings();
        hdrTiming.emplace(device, list, timingSettings.Enabled && !cfg.DlssNrUseProxy.value_or_default(),
                          timingSettings.Interval);

        Barrier(list, g_bbCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        hdrTiming->Begin();
        hdrDecoded = g_presentHdr->Decode(list, g_bbCopy, g_hdrWork, hdrShaderProfile);
        hdrTiming->End();
        PresentHdrUavBarrier(list, g_hdrWork);
        Barrier(list, g_bbCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // What every present host says about a present frame, in one place. A capture from the game's own
    // upscaler has already filled the guide description above; this fills the rest and does not
    // touch it.
    {
        const DlssNrFrameInfo defaults = PresentFrameDefaults(frame.Reset, presentProfile);
        frame.PresentSource = defaults.PresentSource;
        frame.ColourIsLinearHdr = defaults.ColourIsLinearHdr;
        frame.ExposureTexture = defaults.ExposureTexture;
        frame.PreExposure = defaults.PreExposure;
        frame.ExtentIsStable = defaults.ExtentIsStable;
        frame.OutputState = defaults.OutputState;
        frame.PresentWhite = presentHdr ? presentWhite.nits / 80.0f : defaults.PresentWhite;
        frame.PresentQuantRel = defaults.PresentQuantRel;
    }

    // [DlssNr] SynthMotion: with no game guides, motion estimated from this frame and the last in place
    // of zero motion. Read from the copy, before the pass edits it in place; depth stays the zero guide.
    // On an HDR frame the copy is the original codes, which is what it reads (hdr-present-conversion.md, question 5).
    if (!temporalValid && cfg.DlssNrSynthMotion.value_or_default())
    {
        if (g_presentMotion == nullptr)
            g_presentMotion = new SynthMotionGuide();

        // Built for another extent: a present list still in flight may read what it holds.
        if (g_presentMotion->NeedsRebuild(width, height, bbDesc.Format) && WaitPresentListsIdle(2000))
            g_presentMotion->Release();

        if (auto* synthesized = g_presentMotion->Record(device, list, g_bbCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                        false, frame, "D3D12 present", queue))
        {
            motion = synthesized;
            frame.MotionState = GuideRestState(true);
        }
    }

    // What a frame without the game's guides ran on, for the status (HookStatus, the menu's guides line).
    if (!temporalValid)
        NotePresentGuideFallback(motion != s_dummyGuides.Motion());

    if (fgHook)
        g_lastFgFlip = g_presentFlip;

    // Through the shared entry rather than straight into Dispatch. Two doors to the same pass is how
    // the two present transports drifted: ExtentIsStable and the reason string each existed on one
    // side only, on paths facing the identical situation.
    // On an HDR frame the pass runs on the decode, and only if the decode was recorded.
    const char* reason = "";
    bool recorded = false;
    if (presentHdr && !hdrDecoded)
        reason = "the HDR conversion could not record its decode";
    else
        recorded = EvaluateAtPresent(list, presentHdr ? g_hdrWork : g_bbCopy, depth, motion, frame, queue, &reason);
    if (recorded)
        ++g_passesRecorded;

    // The field goes back where the estimator keeps it, on this same list, whether the pass ran or not.
    if (g_presentMotion != nullptr)
        g_presentMotion->AfterPass(list);

    // The one status line, on its own cadence. present:render is the multiplier that actually reached
    // the screen -- the number a session spends the most effort reconstructing by hand otherwise.
    ReportRuntimeStatus(recorded, reason);

    if (g_nr.failed)
    {
        if (trackedSlot < DlssNr::PresentGuides::kRingSize)
        {
            std::lock_guard<std::mutex> lock(g_nrMutex);
            g_presentGuideClones[trackedSlot].readUsage = savedReadUsage;
            g_presentGuideClones[trackedSlot].presentReadUsage = savedPresentReadUsage;
            g_presentGuideClones[trackedSlot].readTracked = savedReadTracked;
        }
        hdrTiming.reset(); // resolves on the list while it is still open
        DropPresentList(slot);
        SettlePresentMotion(false);
        settleDummyGuides(false);
        backbuffer->Release();
        device->Release();
        return;
    }

    {
        static bool firstLogged = false;
        if (!firstLogged)
        {
            firstLogged = true;
            LOG_INFO("DLSS-NR present: first pass on the backbuffer ({}x{}, {} hook)", width, height,
                     fgHook ? "frame-generation" : "swapchain");
        }
    }

    // HDR: the work texture back into the swapchain's format, with the original as the reference, and that is what
    // goes back. A frame the pass did not run on was never edited, so the copy -- the original codes -- goes back as it
    // does on SDR, and a failed encode does the same rather than show what g_hdrOut held before.
    ID3D12Resource* writeBack = g_bbCopy;
    if (presentHdr && recorded)
    {
        Barrier(list, g_hdrWork, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list, g_bbCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        hdrTiming->Begin();
        const bool encoded = g_presentHdr->Encode(list, g_bbCopy, g_hdrWork, g_hdrOut, hdrShaderProfile);
        hdrTiming->End();
        hdrTiming->Finish(encoded);
        Barrier(list, g_bbCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(list, g_hdrWork, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (encoded)
            writeBack = g_hdrOut;
    }
    hdrTiming.reset();

    Barrier(list, writeBack, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, backbuffer, bbState, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(backbuffer, writeBack);
    Barrier(list, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, bbState);
    Barrier(list, writeBack, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const bool executed = SubmitPresentList(queue, slot);
    const bool convertedHdr = presentHdr && executed && writeBack == g_hdrOut;
    if (presentHdr)
        NotePresentHdr(presentProfile, convertedHdr ? nullptr : "HDR conversion did not produce a submitted frame",
                       convertedHdr, presentWhite.nits, presentWhite.fromDisplay);
    // Only claim conversion after decode/model/encode and submission succeeded. Refresh the line on white changes.
    static PresentColour loggedProfile = PresentColour::Sdr;
    static DlssNr::PresentHdr::WhiteLevel loggedWhite;
    if (convertedHdr && (presentProfile != loggedProfile || presentWhite.nits != loggedWhite.nits ||
                         presentWhite.fromDisplay != loggedWhite.fromDisplay))
    {
        loggedProfile = presentProfile;
        loggedWhite = presentWhite;
        LOG_INFO("DLSS-NR present: HDR conversion on, {} at {}x{}: linear Rec.709 FP16, SDR white {:.1f} nits "
                 "({}) x Paper white; original codes bound for reconstruction",
                 presentProfile == PresentColour::Hdr10 ? "HDR10 (PQ, Rec.2020)" : "scRGB (FP16, linear)", width,
                 height, presentWhite.nits, DlssNr::PresentHdr::WhiteSource(presentWhite));
    }
    else if (!presentHdr && loggedProfile != PresentColour::Sdr)
    {
        loggedProfile = PresentColour::Sdr;
        LOG_INFO("DLSS-NR present: HDR conversion off, the swapchain is SDR");
    }
    SettlePresentMotion(executed);
    settleDummyGuides(executed);
    if (!executed && trackedSlot < DlssNr::PresentGuides::kRingSize)
    {
        std::lock_guard<std::mutex> lock(g_nrMutex);
        g_presentGuideClones[trackedSlot].readUsage = savedReadUsage;
        g_presentGuideClones[trackedSlot].presentReadUsage = savedPresentReadUsage;
        g_presentGuideClones[trackedSlot].readTracked = savedReadTracked;
    }

    // Through the unhooked GetModuleHandleW: OptiScaler's own hook answers nvngx_dlssg.dll with itself.
    static bool dlssgLoaded = false;
    if (!dlssgLoaded)
        dlssgLoaded = KernelBaseProxy::GetModuleHandleW_()(L"sl.dlss_g.dll") != nullptr ||
                      KernelBaseProxy::GetModuleHandleW_()(L"nvngx_dlssg.dll") != nullptr;
    const auto& state = State::Instance();
    DlssNr::PresentSyncInputs syncInputs;
    syncInputs.fgHook = fgHook;
    syncInputs.fgOutput = state.activeFgOutput != FGOutput::NoFG;
    syncInputs.dlssgSeen = state.dlssgDetectedInterpolationCount > 0 || state.dlssgLastSetMode != sl::DLSSGMode::eOff;
    syncInputs.dlssgLoaded = dlssgLoaded;
    syncInputs.boundary = boundary;
    const bool presentSync = DlssNr::PresentSyncWanted(cfg.DlssNrPresentSync, syncInputs);
    DlssNr::g_presentSyncWaits.store(presentSync, std::memory_order_relaxed);

    if (presentSync && g_presentList.fenceValue[slot] != 0 &&
        g_presentList.fence->GetCompletedValue() < g_presentList.fenceValue[slot])
    {
        const auto waitStart = std::chrono::steady_clock::now();
        const UINT64 done = g_presentList.fenceValue[slot];

        if (SUCCEEDED(g_presentList.fence->SetEventOnCompletion(done, g_presentList.fenceEvent)))
        {
            const DWORD wait = WaitForSingleObject(g_presentList.fenceEvent, 1000);
            const double waitedMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();

            if (wait != WAIT_OBJECT_0)
                LOG_WARN("DLSS-NR present: completion wait returned {} after {:.1f} ms (slot {})", (unsigned) wait,
                         waitedMs, slot);
        }
    }

    backbuffer->Release();
    device->Release();
}

// ReportSpatialContract, GatherGuideFrame and GatherFrame live in DlssNr_Dx12_Inputs.cpp.

// Describes this frame for the replay recorder and hands it the three surfaces in the states the pass will find them
// in: the after-upscale arrival rules Dispatch applies (OutputState / DepthState / MotionState, else the config's
// resource barriers, else UNORDERED_ACCESS for the output and NON_PIXEL_SHADER_RESOURCE for the guides).
static void RecordReplayFrame(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, ID3D12Resource* target,
                              ID3D12Resource* depth, ID3D12Resource* motion, const DlssNrFrameInfo& frame,
                              bool isRayReconstruction)
{
    const Config& cfg = *Config::Instance();
    const auto outputState = frame.OutputState >= 0 ? static_cast<D3D12_RESOURCE_STATES>(frame.OutputState)
                             : cfg.OutputResourceBarrier.has_value()
                                 ? static_cast<D3D12_RESOURCE_STATES>(cfg.OutputResourceBarrier.value())
                                 : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const auto depthState =
        frame.DepthState >= 0 ? static_cast<D3D12_RESOURCE_STATES>(frame.DepthState)
                              : static_cast<D3D12_RESOURCE_STATES>(
                                    cfg.DepthResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    const auto motionState = frame.MotionState >= 0 ? static_cast<D3D12_RESOURCE_STATES>(frame.MotionState)
                                                    : static_cast<D3D12_RESOURCE_STATES>(cfg.MVResourceBarrier.value_or(
                                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

    DlssNr::ReplayRecorder::BoundaryFacts facts;
    facts.afterUpscaleD3D12 = true;
    facts.exposureTextureBound = frame.ExposureTexture != nullptr;
    auto& meta = facts.meta;
    meta.present = State::Instance().frameCount;
    meta.rayReconstruction = isRayReconstruction;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &meta.createFlags);
    meta.reset = frame.Reset;
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &meta.jitterX);
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &meta.jitterY);
    params->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &meta.frameTimeMs);
    meta.mvScaleX = frame.MvScaleX;
    meta.mvScaleY = frame.MvScaleY;
    meta.preExposure = frame.PreExposure;
    meta.renderWidth = frame.RenderSubrectWidth;
    meta.renderHeight = frame.RenderSubrectHeight;
    meta.depthBaseX = frame.DepthSubrectBaseX;
    meta.depthBaseY = frame.DepthSubrectBaseY;
    meta.motionBaseX = frame.MotionSubrectBaseX;
    meta.motionBaseY = frame.MotionSubrectBaseY;
    DlssNr::ReplayRecorder::AtBoundary(cmdList, target, outputState, depth, depthState, motion, motionState, facts);
}

// Several views in one colour allocation within a frame (DlssNr_MultiView.h; janblade's 1105908f, detector only):
// a multi-monitor or split-screen layout. One model and one history cannot serve several pictures, so every evaluate of
// such a frame is stood down, counted, and the layout's arrival and departure are logged. Both D3D12 seams ask; the
// detector counts one view once. Read-only on the parameter block; the frame is the observed present.
static DlssNr::MultiView::Detector s_multiViews;

static const char* MultiViewStandDown(NVSDK_NGX_Parameter* params, ID3D12Resource* colour)
{
    if (params == nullptr || colour == nullptr)
        return nullptr;

    const D3D12_RESOURCE_DESC desc = colour->GetDesc();
    unsigned int width = 0, height = 0, baseX = 0, baseY = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &baseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &baseY);

    // No rectangle given: the whole allocation.
    if (width == 0 && height == 0)
    {
        width = static_cast<unsigned int>(desc.Width);
        height = desc.Height;
    }

    DlssNr::MultiView::Seen seen;
    seen.allocWidth = desc.Width;
    seen.allocHeight = desc.Height;
    seen.allocFormat = static_cast<uint32_t>(desc.Format);
    seen.rect = { baseX, baseY, width, height };
    const auto result = s_multiViews.Note(State::Instance().frameCount, seen);

    if (result.latchedNow)
        LOG_WARN("DLSS-NR: the game draws several views into one {}x{} colour allocation (a multi-monitor or "
                 "split-screen layout; this view {}x{} at {},{}); NR stands down on every evaluate while it does",
                 seen.allocWidth, seen.allocHeight, width, height, baseX, baseY);
    if (result.unlatchedNow)
        LOG_INFO("DLSS-NR: one view per frame again; NR runs as usual ({} evaluates were stood down)",
                 result.standDowns);
    if (result.verdict != DlssNr::MultiView::Verdict::StandDown)
        return nullptr;

    if (result.standDowns == 1 || result.standDowns % 3600 == 0)
        LOG_INFO("DLSS-NR: {} evaluates stood down so far for a multi-view layout", result.standDowns);
    return "several views are drawn into one colour allocation (multi-monitor or split screen); NR stands down";
}

// Reads the game's parameter block and runs the pass on what it finds.
//
// This is the call site's job, not the pass's. A caller that has the resources in hand -- a
// reprojection stage, a frame generation path, anything that is not the upscaler seam -- calls
// RunPass directly and never touches an NGX parameter block.
void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue, bool preUpscaleDeclined, bool isRayReconstruction)
{
    const Config& cfg = *Config::Instance();
    const bool dlssNrEnabled = cfg.DlssNrEnabled.value_or_default();
    const int dlssNrStage = cfg.DlssNrStage.value_or_default();

    if (!dlssNrEnabled)
    {
        g_resetOnReturn = true;
        GpuTiming::SetEnabled(false);
        ReportSkipOnce("it is switched off");
        return;
    }

    if (isRayReconstruction && !cfg.DlssNrApplyAfterRR.value_or_default())
    {
        ReportSkipOnce("Ray Reconstruction is active; enable ApplyAfterRR to process its output");
        return;
    }

    // Ray reconstruction refines which after-placement this is; it never turns an inactive scope
    // active, so the stand-down case keeps reporting nothing.
    CoverageScope coverage(dlssNrStage != 1     ? (isRayReconstruction ? 3 : 0)
                           : preUpscaleDeclined ? (isRayReconstruction ? 3 : 2)
                                                : -1);

    if (cmdList == nullptr || params == nullptr)
    {
        coverage.sample.reason = "no command list or parameter block";
        ReportSkipOnce("no command list or no parameter block");
        return;
    }

    // Stage 1 runs the model before the upscaler, and this is after it. The caller says whether the
    // pre-upscale scope it wrapped around this same evaluate declined it for a reason this path can
    // serve -- ray reconstruction, a colour it could not copy -- and only then does this path run
    // under stage 1. Otherwise the model would run twice a frame, on both sides of the upscaler.
    //
    // The answer travels with the evaluate rather than through a global on purpose. Jedi Survivor's
    // frame generation evaluates on its own command list, interleaved with the upscaler's, and a
    // global flag written by one evaluate was read by the other: the after-upscale pass ran, the
    // model was rebuilt at display size, and rebuilt again at render size the next frame.
    if (dlssNrStage == 1 && !preUpscaleDeclined)
    {
        ReportSkipOnce("stage 1: the model runs before the upscaler, so the after-upscale pass stands down");
        return;
    }

    // Which of the game's APIs this evaluate arrived through.
    //
    // Says out loud what was previously only reasoned about: an FSR or XeSS title reaches this pass
    // transitively, because those shims call OptiScaler's own NVSDK_NGX_D3D12_EvaluateFeature and
    // this pass hangs off that. Nothing needed adding to the shims -- a call there would run the
    // model twice -- but "nothing needed adding" is a claim, and this is the line that checks it.
    {
        static ApiUpscalerInput saidApi = (ApiUpscalerInput) -1;
        const ApiUpscalerInput api = State::Instance().currentInputApiName;

        if (saidApi != api)
        {
            saidApi = api;
            LOG_INFO("DLSS-NR reached through the game's {} input", ApiUpscalerInputName(api));
        }
    }

    ID3D12Resource* target = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    // Without all three there is nothing to run on. This is not a failure -- some evaluates legitimately
    // carry none of it -- so it stays quiet and tries again next frame.
    if (target == nullptr || depth == nullptr || motion == nullptr)
    {
        coverage.sample.reason = "missing output, depth or motion vectors";
        ReportSkipOnce(target == nullptr  ? "the parameters carried no output texture"
                       : depth == nullptr ? "the parameters carried no depth"
                                          : "the parameters carried no motion vectors");
        return;
    }

    DlssNrFrameInfo frame = GatherFrame(params);
    frame.AfterRayReconstruction = isRayReconstruction;
    const auto contractOutput = target->GetDesc();
    ReportSpatialContract(params, GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color"), target, false,
                          static_cast<unsigned>(contractOutput.Width), contractOutput.Height);

    // Auto that has seen the game's own DLSS-G stays here (DlssNr_PresentBoundary.h).
    const auto boundary = BoundaryInputs(cfg.DlssNrHookMethod.value_or_default(), false);
    if (DlssNr::PresentBoundary::UpscaledCapturesForPresent(boundary, PresentHookLive()))
        CaptureTemporal(cmdList, params, frame);

    if (DlssNr::PresentBoundary::UpscaledDefersToPresent(boundary, PresentHookLive(), g_temporalValid.load()))
    {
        coverage.sample.reason = "deferred to present-time hook";
        return;
    }

    // From here the frame is this route's: count it as a render and publish how it ends.
    ++g_upscaleRenders;
    g_upscaleApplied = false;
    g_upscaleReason = coverage.sample.reason;

    // Several views in one colour allocation (DlssNr_MultiView.h): nothing is recorded on any of them, and the history
    // is owed a reset for when the layout ends.
    if (const char* why = MultiViewStandDown(params, GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color")))
    {
        coverage.sample.reason = why;
        g_upscaleReason = why;
        g_resetOnReturn = true;
        ReportSkipOnce(why);
        return;
    }

    if (g_sourceIsPresent.exchange(false))
        g_resetOnReturn = true;

    // The upscaler's inputs are at render resolution while colour and output are at display
    // resolution; the model takes that as a subrect per resource, which the pass reads from the
    // resources themselves.
    //
    // The device is the command list's: the model is created and evaluated on it, and under a wrapper layer the
    // output reports the device underneath instead (DlssNr_DeviceMatch.h).
    ID3D12Device* device = nullptr;

    if (FAILED(cmdList->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        coverage.sample.reason = "command list has no D3D12 device";
        g_upscaleReason = coverage.sample.reason;
        ReportSkipOnce("the command list belongs to no D3D12 device");
        return;
    }

    // The pass is the object, so the caller holds it. Built once, on the device the frame is on.
    const bool composeReady = DeviceChildBelongsTo(device, target) && DeviceChildBelongsTo(device, depth) &&
                              DeviceChildBelongsTo(device, motion) &&
                              (frame.ExposureTexture == nullptr ||
                               DeviceChildBelongsTo(device, static_cast<ID3D12Resource*>(frame.ExposureTexture))) &&
                              EnsureCompose(device);
    device->Release();

    if (!composeReady)
    {
        coverage.sample.reason = "composition device unavailable or changed";
        g_upscaleReason = coverage.sample.reason;
        ReportSkipOnce("the pass could not be created");
        return;
    }

    // The replay recorder (offline-replay-recorder.md): the frame as the pass receives it, copied before it writes.
    // Inert until a clip is first asked for: Poll and Wanted are one relaxed load each.
    DlssNr::ReplayRecorder::Poll();
    if (DlssNr::ReplayRecorder::Wanted())
        RecordReplayFrame(cmdList, params, target, depth, motion, frame, isRayReconstruction);

    const auto desc = target->GetDesc();
    coverage.sample.width = static_cast<uint32_t>(desc.Width);
    coverage.sample.height = desc.Height;
    coverage.sample.applied =
        g_compose->Dispatch(cmdList, target, depth, motion, target, frame, timingQueue, &coverage.sample);
    if (coverage.sample.applied)
        coverage.sample.reason = "composition recorded into upscaler output";
    g_upscaleApplied = coverage.sample.applied;
    g_upscaleReason = coverage.sample.reason;
    if (coverage.sample.applied)
        ++g_passesRecorded;
}

// ---------------------------------------------------------------------------------------------
// At present, with no upscaler anywhere in the frame.
// ---------------------------------------------------------------------------------------------

// GuideRestState lives in DlssNr_Dx12_Inputs.cpp.

namespace
{
// Where the estimator leaves its field between frames (SynthMotion::Estimator_Dx12's contract).
constexpr D3D12_RESOURCE_STATES kEstimatorMotionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// A gap this long between two recordings means the frame the estimator last saw is not the previous
// frame -- the pass was off, the key toggled, the host rebuilt -- and differencing against it would
// be motion between two unrelated pictures. Well above any frame time a present host runs at.
constexpr long long kSynthMotionStaleMs = 250;

// [DlssNr] SynthMotionSource, resolved. auto is the FidelityFX estimator until the engine has been
// measured against it in a game (synthesized-motion.md, "Motion sources").
bool WantsNvofaMotion()
{
    const std::string source = Config::Instance()->DlssNrSynthMotionSource.value_or_default();
    if (source == "nvofa")
        return true;

    static bool reportedUnknown = false;
    if (source != "auto" && source != "ffx" && !reportedUnknown)
    {
        reportedUnknown = true;
        LOG_WARN("DLSS-NR synthesized motion: SynthMotionSource={} is not auto, ffx or nvofa; using ffx", source);
    }

    return false;
}
} // namespace

SynthMotionGuide::SynthMotionGuide() = default;
SynthMotionGuide::~SynthMotionGuide() { Release(); }

bool SynthMotionGuide::NeedsRebuild(uint32_t width, uint32_t height, DXGI_FORMAT format) const
{
    return (_estimator != nullptr || _nvofa != nullptr) && (_width != width || _height != height || _format != format);
}

ID3D12Resource* SynthMotionGuide::Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                                         ID3D12Resource* colour, D3D12_RESOURCE_STATES colourState, bool reset,
                                         DlssNrFrameInfo& frame, const char* route, ID3D12CommandQueue* queue)
{
    _lastSynthesized = false;

    if (!Config::Instance()->DlssNrSynthMotion.value_or_default() || device == nullptr || cmdList == nullptr ||
        colour == nullptr)
    {
        return nullptr;
    }

    const auto desc = colour->GetDesc();
    const auto width = static_cast<uint32_t>(desc.Width);
    const auto height = desc.Height;

    // Built for another frame, and the caller has not yet proved the GPU is done with it. Nothing is
    // recorded over resources that may still be in flight; the caller keeps its zero guide meanwhile.
    if (NeedsRebuild(width, height, desc.Format))
        return nullptr;

    // A field left handed out by a frame that executed without AfterPass sits in the pass's rest
    // state, not the estimator's. That is a caller bug; nothing more is recorded until a Release().
    if (_handedOut)
    {
        if (!_reportedFailure)
        {
            _reportedFailure = true;
            LOG_ERROR("DLSS-NR synthesized motion: the field was not handed back after the pass on the {} route; "
                      "zero motion until the next rebuild",
                      route);
        }

        return nullptr;
    }

    ID3D12Resource* motion = nullptr;
    bool sceneCut = false;
    SynthMotion::Handoff::Field shared {};

    // Synthesized frame generation may have estimated this base frame already, on this device, at this
    // extent (native D3D12: its feed runs before this pass). Its field is the same estimate; taking it
    // costs nothing. Our own estimator then sits idle, and the staleness rule below resets it when it
    // is next needed.
    if (SynthMotion::Handoff::Take(SynthMotion::Handoff::Owner::DlssNr, device, width, height, &shared))
    {
        _peerWait = {};
        motion = shared.motion;
        sceneCut = shared.sceneCut;

        if (!_reportedTaken)
        {
            _reportedTaken = true;
            LOG_INFO("DLSS-NR synthesized motion: field taken from frame generation's estimate of the frame on the {} "
                     "route ({}x{}), no second estimate",
                     route, width, height);
        }
    }
    else if (SynthMotion::Handoff::WaitForPeer(
                 _peerWait, _estimator != nullptr || _nvofa != nullptr,
                 SynthMotion::Handoff::PeerWarming(SynthMotion::Handoff::Owner::DlssNr, device, width, height),
                 NowMs()))
    {
        // Frame generation's estimator recorded this frame and is still warming up (native D3D12 at start-up:
        // its feed runs before this pass). A second one would warm over the same frames and then sit idle
        // once frame generation publishes, so nothing is recorded, and the caller keeps its zero guide --
        // all a warming estimator would have given.
        if (!_reportedPeerWait)
        {
            _reportedPeerWait = true;
            LOG_INFO("DLSS-NR synthesized motion: frame generation's estimate is warming up on the {} route ({}x{}); "
                     "waiting for it rather than running a second estimator",
                     route, width, height);
        }

        return nullptr;
    }
    else
    {
        if (_estimator == nullptr && _nvofa == nullptr)
        {
            if (_peerWait.sinceMs >= 0)
            {
                LOG_INFO("DLSS-NR synthesized motion: stopped waiting for frame generation's estimate on the {} route "
                         "after {} ms, {} fresh frame(s) in a row ({}); running our own",
                         route, NowMs() - _peerWait.sinceMs, _peerWait.freshFrames,
                         SynthMotion::Handoff::PeerWarming(SynthMotion::Handoff::Owner::DlssNr, device, width, height)
                             ? "still warming"
                             : "no longer announced");
                _peerWait = {};
            }

            if (!_nvofaRefused && queue != nullptr && WantsNvofaMotion())
            {
                _nvofa = std::make_unique<SynthMotion::NvofaEstimator_Dx12>();
                LOG_INFO("DLSS-NR synthesized motion: NVIDIA Optical Flow selected on the {} route", route);
            }
            else
            {
                _estimator = std::make_unique<SynthMotion::Estimator_Dx12>();
            }
        }

        const long long now = NowMs();
        const bool stale = _lastRecordMs == 0 || now - _lastRecordMs > kSynthMotionStaleMs;
        _lastRecordMs = now;

        bool recorded = false;
        bool fellBack = false;
        if (_nvofa != nullptr)
        {
            recorded = _nvofa->Record(device, cmdList, colour, colourState, reset || stale, queue);

            // Unavailable is decided before anything is recorded, so this list is still clean: the
            // FidelityFX estimator starts over on it. The engine's previous list may still be in flight, so
            // it is retired rather than released.
            if (!recorded && !_nvofa->Available())
            {
                LOG_WARN("DLSS-NR synthesized motion: NVIDIA Optical Flow is unavailable on the {} route ({}); using "
                         "the FidelityFX estimator for the session",
                         route, _nvofa->UnavailableReason());
                _nvofaRefused = true;
                _retiredNvofa = std::move(_nvofa);
                _estimator = std::make_unique<SynthMotion::Estimator_Dx12>();
                fellBack = true;
            }
        }

        if (_estimator != nullptr)
            recorded = _estimator->Record(device, cmdList, colour, colourState, reset || stale || fellBack);

        if (!recorded)
        {
            if (!_reportedFailure)
            {
                _reportedFailure = true;
                LOG_WARN("DLSS-NR synthesized motion: the estimator did not record on the {} route ({}x{}); the "
                         "model keeps zero motion",
                         route, width, height);
            }

            return nullptr;
        }

        _recorded = true;
        _width = width;
        _height = height;
        _format = desc.Format;

        // Nothing to compare against yet -- the first frame, or one after a reset -- or no field at all.
        if (_nvofa != nullptr)
        {
            motion = _nvofa->Ready() ? _nvofa->Motion() : nullptr;
            sceneCut = _nvofa->SceneCut();
        }
        else
        {
            motion = _estimator->Ready() ? _estimator->Motion() : nullptr;
            sceneCut = _estimator->SceneCut();
        }

        if (motion == nullptr)
        {
            // Warming up. Frame generation, when it runs after this on the same frame (the D3D11 bridge), waits
            // for this estimator rather than build its own. Not for the optical-flow engine, whose fields are
            // never published (below).
            if (_nvofa == nullptr)
                SynthMotion::Handoff::AnnounceWarming(SynthMotion::Handoff::Owner::DlssNr, device, width, height);

            return nullptr;
        }

        // Frame generation, when it runs after this on the same frame (the D3D11 bridge), takes this field.
        // Only a field of this frame: the optical-flow engine's arrives one frame late, and the handoff
        // promises the taker the current base frame, so frame generation estimates its own instead.
        if (_nvofa == nullptr)
            SynthMotion::Handoff::Publish(SynthMotion::Handoff::Owner::DlssNr, device, motion, width, height, sceneCut);
    }

    // The pass transitions a motion guide from GuideRestState(true), so the field is handed over there.
    // The same state in the default config, when this is no barrier at all.
    _handedOutState = static_cast<D3D12_RESOURCE_STATES>(GuideRestState(true));
    Barrier(cmdList, motion, kEstimatorMotionState, _handedOutState);
    _handedOut = true;
    _handedOutResource = motion;

    // Full extent, output resolution, in pixels: nothing about it is at render size or in a subrect.
    frame.MvScaleX = kSynthMotionScaleX;
    frame.MvScaleY = kSynthMotionScaleY;
    frame.MotionVectorsLowResolution = false;
    frame.MotionSubrectBaseX = 0;
    frame.MotionSubrectBaseY = 0;

    // Real motion leaves the zero-guide policy reset (ZeroGuideReset) nothing to protect against. A
    // genuine reset the caller already carried stays.
    if (frame.ResetIsPolicy)
    {
        frame.Reset = false;
        frame.ResetIsPolicy = false;
    }

    // A cut is a cut, whoever notices it.
    if (sceneCut)
    {
        frame.Reset = true;
        frame.ResetIsPolicy = false;
    }

    _lastSynthesized = true;

    if (!_reportedUse)
    {
        _reportedUse = true;
        LOG_INFO("DLSS-NR synthesized motion: first field handed to the model on the {} route ({}x{}, motion vector "
                 "scale {} x {})",
                 route, width, height, kSynthMotionScaleX, kSynthMotionScaleY);
    }

    return motion;
}

void SynthMotionGuide::AfterPass(ID3D12GraphicsCommandList* cmdList)
{
    if (!_handedOut || _handedOutResource == nullptr || cmdList == nullptr)
        return;

    // Our own field or frame generation's: either way back where the estimator that made it keeps it.
    Barrier(cmdList, _handedOutResource, _handedOutState, kEstimatorMotionState);
    _handedOut = false;
    _handedOutResource = nullptr;
}

void SynthMotionGuide::ConfirmExecuted()
{
    if (!_recorded || (_estimator == nullptr && _nvofa == nullptr))
        return;

    if (_nvofa != nullptr)
        _nvofa->ConfirmExecuted();
    else
        _estimator->ConfirmExecuted();
    _recorded = false;
}

void SynthMotionGuide::AbandonRecording()
{
    // Both barriers, out and back, were on the list nobody ran: the field is where the estimator left it,
    // whether it was ours or taken.
    _handedOut = false;
    _handedOutResource = nullptr;

    if (!_recorded || (_estimator == nullptr && _nvofa == nullptr))
        return;

    // Published for a list nobody ran: nobody may take it.
    if (_nvofa != nullptr)
    {
        SynthMotion::Handoff::Withdraw(_nvofa->Motion());
        _nvofa->AbandonRecording();
    }
    else
    {
        SynthMotion::Handoff::Withdraw(_estimator->Motion());
        _estimator->AbandonRecording();
    }
    _recorded = false;
}

void SynthMotionGuide::Release()
{
    SynthMotion::Handoff::WithdrawWarming(SynthMotion::Handoff::Owner::DlssNr);

    if (_estimator != nullptr)
    {
        SynthMotion::Handoff::Withdraw(_estimator->Motion());
        _estimator->Release();
        _estimator.reset();
    }

    for (auto* nvofa : { &_nvofa, &_retiredNvofa })
    {
        if (*nvofa != nullptr)
        {
            SynthMotion::Handoff::Withdraw((*nvofa)->Motion());
            (*nvofa)->Release();
            nvofa->reset();
        }
    }

    _width = 0;
    _height = 0;
    _format = DXGI_FORMAT_UNKNOWN;
    _lastRecordMs = 0;
    _peerWait = {};
    _handedOutState = D3D12_RESOURCE_STATE_COMMON;
    _handedOutResource = nullptr;
    _recorded = false;
    _handedOut = false;
    _lastSynthesized = false;
    _reportedFailure = false;
}

DlssNrFrameInfo PresentFrameDefaults(bool reset, PresentColour colour)
{
    DlssNrFrameInfo frame {};

    // The frame is a backbuffer: whatever the game rendered has already been through its tonemapper,
    // so the encode is the identity. Saying otherwise encodes an encoded frame a second time, which
    // is the washed-out, banded failure the flag exists to prevent. Nothing here reads an exposure
    // for the same reason -- a display-referred frame has none to read.
    frame.PresentSource = true;
    frame.ColourIsLinearHdr = false;
    frame.ExposureTexture = nullptr;
    frame.PreExposure = 1.0f;

    // Colour is read and written in place, as the after-upscale path does, and a present host owns
    // that resource and rests it in UNORDERED_ACCESS.
    frame.OutputState = (int) D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // The swapchain's extent changes on resize and at no other time, and a resize rebuilds everything
    // a present host owns, so the post-upscale settling gate has nothing to protect here.
    frame.ExtentIsStable = true;

    // What zero guides describe, and the truthful answer when there is nothing captured: guides the
    // size of the frame, no subrect, no motion encoding. A caller with real guides overwrites these.
    frame.MotionVectorsLowResolution = false;
    frame.DepthInverted = false;
    frame.MvScaleX = 1.0f;
    frame.MvScaleY = 1.0f;

    // History has nothing to carry over on the first frame, after a resize, and after any gap the
    // host could not bridge. The caller knows which of those happened; this side cannot.
    frame.Reset = reset;

    // An HDR swapchain's frame, which its host decoded into linear Rec.709 in scRGB units before this
    // (docs/dlssnr/design/hdr-present-conversion.md): the linear-HDR path the pass was built for, the display's
    // SDR white as the white point's base, and on HDR10 a PQ code as the output stabilizer's unit. Still no
    // exposure texture: the white is arithmetic, not a reading.
    if (IsHdrProfile(colour))
    {
        frame.ColourIsLinearHdr = true;
        frame.PresentWhite = DlssNr::PresentHdr::kSdrWhiteScRgb;
        frame.PresentQuantRel = colour == PresentColour::Hdr10 ? DlssNr::PresentHdr::kPqQuantRel : 0.0f;
    }

    return frame;
}

bool EvaluateAtPresent(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* depth,
                       ID3D12Resource* motion, const DlssNrFrameInfo& frame, ID3D12CommandQueue* timingQueue,
                       const char** outReason)
{
    const auto answer = [outReason](bool recorded, const char* reason)
    {
        if (outReason != nullptr)
            *outReason = reason;
        return recorded;
    };

    if (!Config::Instance()->DlssNrEnabled.value_or_default())
    {
        g_resetOnReturn = true;
        DlssNr::SessionGuardOff();
        return answer(false, "the pass is disabled");
    }

    if (cmdList == nullptr || colour == nullptr || depth == nullptr || motion == nullptr)
        return answer(false, "the host passed a null resource");

    // The host's list decides the device; its colour may report the device under a wrapper (DlssNr_DeviceMatch.h).
    ID3D12Device* device = nullptr;
    if (FAILED(cmdList->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the present host's command list belongs to no D3D12 device");
        return answer(false, "the command list belongs to no D3D12 device");
    }

    const bool composeReady = DeviceChildBelongsTo(device, colour) && DeviceChildBelongsTo(device, depth) &&
                              DeviceChildBelongsTo(device, motion) &&
                              (frame.ExposureTexture == nullptr ||
                               DeviceChildBelongsTo(device, static_cast<ID3D12Resource*>(frame.ExposureTexture))) &&
                              EnsureCompose(device);
    device->Release();

    if (!composeReady)
    {
        ReportSkipOnce("the present host inputs or composition cache belong to another device");
        return answer(false, "the composition pass could not be built");
    }

    const auto desc = colour->GetDesc();

    DlssNr::Detail::CoverageSample sample {};
    sample.width = static_cast<uint32_t>(desc.Width);
    sample.height = desc.Height;

    const bool recorded = g_compose->Dispatch(cmdList, colour, depth, motion, colour, frame, timingQueue, &sample);

    if (recorded)
        return answer(true, "composition recorded at present, with no upscaler in the frame");

    // Dispatch's own account of why not. It distinguishes a model that was called and refused from a
    // frame the pass chose not to serve, which is the distinction the caller needs and the one a bare
    // bool destroys.
    return answer(false, sample.reason);
}

// ---------------------------------------------------------------------------------------------
// Before the upscaler.
// ---------------------------------------------------------------------------------------------

ScopedPreUpscale::ScopedPreUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, bool applies,
                                   ID3D12CommandQueue* timingQueue)
    : _cmdList(cmdList), _params(params)
{
    const Config& cfg = *Config::Instance();
    const bool holdFrame = cfg.DlssNrHoldFrame.value_or_default();
    const bool useProxy = cfg.DlssNrUseProxy.value_or_default();
    const int stage = cfg.DlssNrStage.value_or_default();
    const bool enabled = cfg.DlssNrEnabled.value_or_default();

    if (!enabled)
    {
        g_resetOnReturn = true;
        DlssNr::SessionGuardOff();
    }

    if (!enabled || stage != 1)
        return;

    // Present owns the frame whatever the stage; the menu hides Stage under Present for that reason.
    // This scope never read the hook method, so Stage 1 with Present ran the model here, at render
    // size, and again at present at display size -- and the size alternating every frame reset the
    // model and rebuilt it on each. Declined rather than returned, so the after-upscale call for this
    // evaluate still runs and captures the guides the present pass reprojects with.
    if (cfg.DlssNrHookMethod.value_or_default() == 2)
    {
        _declined = true;
        ReportSkipOnce("before the upscaler: the hook method is Present, which runs the pass at present instead");
        return;
    }

    CoverageScope coverage(1);

    // This route owns the frame from here: one render for the status line, and the outcome it will
    // report. The scope's sample is the truth on every path out of this constructor, so it is
    // published when the scope closes rather than at each return. (Stage 1 read "0 renders" and
    // "no upscaler evaluate yet" before this, because the after-upscale pass stands down at
    // stage 1 and the counting lived only there.)
    ++g_upscaleRenders;
    struct PublishOutcome
    {
        const CoverageScope& scope;
        ~PublishOutcome()
        {
            g_upscaleApplied = scope.sample.applied;
            g_upscaleReason = scope.sample.reason;
            if (scope.sample.applied)
                ++g_passesRecorded;
        }
    } publishOutcome { coverage };

    if (cmdList == nullptr || params == nullptr)
    {
        coverage.sample.reason = "no command list or parameter block";
        return;
    }

    // Several views in one colour allocation (DlssNr_MultiView.h): stood down, not declined. Declining would hand the
    // evaluate to the after-upscale pass, which would run the model over every screen of the output.
    if (const char* why = MultiViewStandDown(params, GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color")))
    {
        coverage.sample.reason = why;
        g_preStatus = why;
        g_resetOnReturn = true;
        ReportSkipOnce(why);
        return;
    }

    _lock = std::unique_lock<std::mutex>(g_preMutex, std::try_to_lock);
    if (!_lock.owns_lock())
    {
        coverage.sample.reason = "nested or concurrent scratch loan";
        return; // Nested or concurrent evaluate: never swap a scratch another scope is lending.
    }

    // Declining hands the evaluate to the after-upscale path; a plain return does not. The
    // distinction is whether the model could still do a sensible job of this frame from the other
    // side, and for ray reconstruction it can: the finished frame is denoised, the input is not.
    // The answer is kept on this scope, so it can only ever reach the after-upscale call for the
    // same evaluate; see EvaluateAfterUpscale for the frame-generation interleaving that made a
    // global flag wrong.
    const auto report = [&coverage](const char* why)
    {
        coverage.sample.reason = why;
        g_preStatus = why;
        ReportSkipOnce(why);
    };
    auto decline = [this, &report, &coverage](const char* why)
    {
        coverage.sample.fallback = true;
        _declined = true;
        g_preStatus = why;
        report(why);
    };

    if (!applies)
    {
        decline("before the upscaler: ray reconstruction's colour is undenoised, so the after-upscale "
                "pass serves those evaluates");
        return;
    }

    if (holdFrame || useProxy)
    {
        decline("before the upscaler: Hold frame and the driver proxy use the after-upscale path");
        return;
    }

    if (State::Instance().frameCount == 0)
    {
        decline("before the upscaler: no observed present boundary; using the after-upscale path");
        return;
    }

    // The colour, and how the block stored it. NVIDIA's block keeps typed and untyped slots apart,
    // OptiScaler's does not; the swap and its undo have to use whichever the game or the bridge used,
    // or the upscaler reads the original from the other slot and nothing changes.
    ID3D12Resource* colour = _color.Read(params, NVSDK_NGX_Parameter_Color, NVSDK_NGX_Result_Success);

    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    if (colour == nullptr || depth == nullptr || motion == nullptr)
    {
        report(colour == nullptr  ? "before the upscaler: the parameters carried no colour"
               : depth == nullptr ? "before the upscaler: the parameters carried no depth"
                                  : "before the upscaler: the parameters carried no motion vectors");
        return;
    }

    const auto displaySize = DlssNr::GetDisplaySize(params);
    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);
    if (!(createFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) && (!displaySize.width || !displaySize.height))
    {
        decline("before the upscaler: display-resolution motion has no known display reference");
        return;
    }

    // What was drawn, as opposed to how big the texture is. A dynamic-resolution game renders into
    // the corner of a buffer allocated once at its largest, and the upscaler is told the drawn size
    // through the render subrect. The copy is that size: the model then works on the picture and
    // not the margin, and the upscaler reads the copy with the same subrect it would have read the
    // original with.
    const D3D12_RESOURCE_DESC colourDesc = colour->GetDesc();
    unsigned int width = (unsigned int) colourDesc.Width;
    unsigned int height = colourDesc.Height;

    {
        const auto [subW, subH] = DlssNr::GetRenderSize(params);

        if ((subW == 0) != (subH == 0))
        {
            decline("before the upscaler: incomplete render subrect dimensions");
            return;
        }
        if (subW == 0 || subH == 0)
        {
            decline("before the upscaler: unknown or stale render geometry");
            return;
        }
        if (subW != 0 && subH != 0)
        {
            if (subW > width || subH > height)
            {
                decline("before the upscaler: render subrect exceeds the color texture");
                return;
            }
            width = subW;
            height = subH;
        }
    }

    // The copy starts at the origin, so the game's colour has to as well. A game that offsets its
    // colour subrect would need the copy offset the same way, which nothing here does yet.
    {
        unsigned int baseX = 0, baseY = 0;
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &baseX);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &baseY);

        if (baseX != 0 || baseY != 0)
        {
            decline("before the upscaler: the game's colour subrect does not start at the origin, "
                    "which the copy does not follow yet; the after-upscale pass serves instead");
            return;
        }
    }

    // A colour with a mip chain. The read scope below moves every subresource from the one state the game declared,
    // while a game that builds the chain may leave the other mips elsewhere, and the pass's views and copies are of
    // one mip. hhkbble's 08fa575e admits typed multi-mip colour by transitioning only mip 0; that is not proved for
    // every use here (the encode's views, NGX's own, the captures), so this stage declines it and the after-upscale
    // pass serves the evaluate (docs/dlssnr/design/dx12-robustness.md). Counted as a fallback with its own reason.
    if (colourDesc.MipLevels != 1)
    {
        decline("before the upscaler: the game's colour has several mip levels; the pass reads and moves one-mip "
                "colour only, so the after-upscale pass serves instead");
        return;
    }

    if (colourDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || colourDesc.SampleDesc.Count != 1 ||
        colourDesc.DepthOrArraySize != 1 || (colourDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        colourDesc.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || IsTypeless(colourDesc.Format))
    {
        decline("before the upscaler: unsupported color texture contract (including ambiguous typeless color)");
        return;
    }

    coverage.sample.width = width;
    coverage.sample.height = height;
    const DXGI_FORMAT format = colourDesc.Format;

    if (IsSrgb(format) || width == 0 || height == 0)
    {
        decline("before the upscaler: the game's colour is an sRGB surface, which cannot be written as "
                "a UAV or read back as linear; the after-upscale pass serves instead");
        return;
    }

    // The game's list decides the device the model and the scratch are made on; under a wrapper layer the colour
    // reports the device underneath (DlssNr_DeviceMatch.h), which the check below accepts and nothing else.
    ID3D12Device* device = nullptr;

    if (FAILED(cmdList->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        report("before the upscaler: the command list belongs to no D3D12 device");
        return;
    }

    if (g_preDevice && g_preDevice != device)
    {
        device->Release();
        // The existing NR state is also device-bound. Neither stage can safely reuse it here.
        report("before the upscaler: device changed; restart the session before enabling NR");
        return;
    }

    if (!g_preExtent.Observe(width, height, std::chrono::steady_clock::now(), static_cast<uint32_t>(format)))
    {
        device->Release();
        g_preStatus = "Waiting for a stable render resolution (500 ms); original color retained";
        report(g_preStatus.load());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_nrMutex);
        if (!DeviceChildBelongsTo(device, colour) || !DeviceChildBelongsTo(device, depth) ||
            !DeviceChildBelongsTo(device, motion) || !g_nrDeviceOwner.Accept(device))
        {
            device->Release();
            report("before the upscaler: inputs or NR caches belong to another device; restart the process");
            return;
        }
        AdvanceNrClock();

        const auto requested = cfg.GetDlssNrPassSnapshot();
        if (WantsTrackedRecording(requested))
            _recording = std::make_unique<Chain::RecordingLease>(g_recordings, reinterpret_cast<uintptr_t>(cmdList));
        if (!TrackNrRecording(cmdList, requested, _recording.get()))
        {
            device->Release();
            report(g_chainStatus.load());
            return;
        }

        if (g_pre.width != width || g_pre.height != height || g_pre.format != format)
        {
            // Parked, not released: the upscaler read it a frame or two ago and the GPU may still
            // be there.
            ParkNrResource(g_pre.scratch);
            g_preAllocationFailed = false;
        }

        if (g_pre.scratch == nullptr && !g_preAllocationFailed)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support { format };
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
                !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
                !(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) ||
                !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            {
                device->Release();
                decline("before the upscaler: color format lacks required shader, render-target or UAV support");
                return;
            }

            g_pre.scratch = CreatePreUpscaleScratch(device, format, width, height);
            g_pre.width = width;
            g_pre.height = height;
            g_pre.format = format;
            g_preAllocationFailed = g_pre.scratch == nullptr;
            if (g_pre.scratch != nullptr)
                g_preDevice = device;

            if (g_pre.scratch != nullptr)
                LOG_INFO("DLSS-NR before the upscaler: the model works on the {}x{} render-size colour "
                         "(format {}, texture {}x{}); the upscaler is handed the edited copy",
                         width, height, (int) format, (unsigned int) colourDesc.Width, colourDesc.Height);
        }
    }

    if (g_pre.scratch == nullptr)
    {
        device->Release();
        decline("before the upscaler: the working copy could not be allocated; the after-upscale pass "
                "serves instead");
        return;
    }

    DlssNrFrameInfo frame = GatherFrame(params);
    ReportSpatialContract(params, colour, GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output"), true, width,
                          height);
    if (colour == depth || colour == motion || depth == motion ||
        (frame.ExposureTexture &&
         (frame.ExposureTexture == colour || frame.ExposureTexture == depth || frame.ExposureTexture == motion)))
    {
        device->Release();
        decline("before the upscaler: aliased color, guide or exposure inputs are unsupported");
        return;
    }
    if (frame.ExposureTexture != nullptr &&
        !DeviceChildBelongsTo(device, static_cast<ID3D12Resource*>(frame.ExposureTexture)))
    {
        device->Release();
        report("before the upscaler: exposure belongs to another device");
        return;
    }
    if (!ExposureTextureUsable(device, static_cast<ID3D12Resource*>(frame.ExposureTexture)))
        frame.ExposureTexture = nullptr;
    frame.OutputState = (int) D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // The colour is read the way NGX reads it, in NON_PIXEL_SHADER_RESOURCE. A game that leaves it
    // elsewhere says so through ColorResourceBarrier, and OptiScaler's upscalers move it from that
    // state before reading and back after. They will not see the game's colour this frame -- they
    // see the copy -- so the move is made here for the game's colour, and the copy is handed over
    // resting in the same state the game's colour rests in, so that their move applies to it.
    const D3D12_RESOURCE_STATES colourRest = cfg.ColorResourceBarrier.has_value()
                                                 ? (D3D12_RESOURCE_STATES) cfg.ColorResourceBarrier.value()
                                                 : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto depthRest = static_cast<D3D12_RESOURCE_STATES>(
        cfg.DepthResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    const auto motionRest = static_cast<D3D12_RESOURCE_STATES>(
        cfg.MVResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    const auto exposureRest = static_cast<D3D12_RESOURCE_STATES>(
        cfg.ExposureResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

    const unsigned int colorStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE |
                                     D3D12_RESOURCE_STATE_COPY_DEST;
    if ((static_cast<unsigned int>(colourRest) & ~colorStates) != 0)
    {
        device->Release();
        decline("before the upscaler: unsupported color arrival state");
        return;
    }

    struct ReadScope
    {
        ID3D12GraphicsCommandList* cmd;
        ID3D12Resource* resource;
        D3D12_RESOURCE_STATES arrival;
        ReadScope(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a)
            : cmd(c), resource(r), arrival(a)
        {
            if (resource)
                Barrier(cmd, resource, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        ~ReadScope()
        {
            if (resource)
                Barrier(cmd, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
        }
    };
    // Each game resource is restored even on a build/skip/failed dispatch. ReadableGuide sees the
    // NPSR state it expects; the upscaler receives its original configured arrival states afterward.
    if (!EnsureCompose(device))
    {
        device->Release();
        report("before the upscaler: composition device changed");
        return;
    }
    ReadScope colorRead(cmdList, colour, colourRest);
    ReadScope depthRead(cmdList, depth, depthRest);
    ReadScope motionRead(cmdList, motion, motionRest);
    ReadScope exposureRead(cmdList, static_cast<ID3D12Resource*>(frame.ExposureTexture), exposureRest);

    device->Release();

    bool wrote = false;

    if (g_compose != nullptr)
        wrote = g_compose->Dispatch(cmdList, colour, depth, motion, g_pre.scratch, frame, timingQueue, &coverage.sample,
                                    _recording.get());

    // Nothing written means nothing to swap: the copy holds stale or no picture, and the upscaler
    // gets the game's own colour, which is the frame as it would have been without the pass. This is
    // every frame until the model is built, and any frame the pass skips.
    if (!wrote)
    {
        report(coverage.sample.reason);
        return;
    }

    Barrier(cmdList, g_pre.scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, colourRest);

    _scratch = g_pre.scratch;
    _color.Swap(_scratch);
    _scratchState = (int) colourRest;
    _swapped = true;
    coverage.sample.applied = true;
    coverage.sample.reason = "composition recorded and edited color selected for upscaler";
    g_preStatus = "Before the upscaler: edited render-resolution color supplied";
}

ScopedPreUpscale::~ScopedPreUpscale()
{
    if (!_swapped)
        return;

    // The game's colour goes back before the block is reused, and the copy goes back to rest where
    // the next frame's pass expects to find it. The list is still recording: the scope closes right
    // after the evaluate it wrapped, before anything is submitted.
    _color.Restore();

    if (_scratch != nullptr)
        Barrier(_cmdList, _scratch, (D3D12_RESOURCE_STATES) _scratchState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// The pass. Resources in, nothing read from anywhere the caller cannot see.

CalibrationReading Calibration()
{
    CalibrationReading r {};
    r.suggestion = g_nr.calibSuggestion;
    r.steadiness = g_nr.calibSteadiness;
    r.samples = g_nr.calibCount;
    r.usable = g_nr.calibUsable;
    r.why = g_nr.calibWhy;
    return r;
}

bool IsRunning() { return g_nr.feature != nullptr && !g_nr.failed; }

const char* FailureReason() { return g_nr.failed ? g_nr.reason : ""; }

// What the game offers by way of exposure, and what has been read from it. For the menu, so a user
// can see whether this game supplies one at all without having to read a log.
ExposureStatus GameExposureStatus()
{
    ExposureStatus s {};
    s.seenFrames = g_nr.exposureFrames;
    s.offeredNow = g_nr.exposureOfferedNow;
    s.everOffered = g_nr.exposureEverOffered;
    s.exposure = g_nr.gameExposure;
    s.preExposure = g_nr.gamePreExposure;
    s.latched = g_nr.exposureLatch.Latched();
    return s;
}

std::optional<double> LastGpuTime() { return std::nullopt; }
const char* BeforeUpscaleStatus() { return g_preStatus.load(); }

bool RunningAfterRayReconstruction()
{
    std::lock_guard<std::mutex> lock(g_nrMutex);
    return g_nr.afterRayReconstruction;
}

bool ObservedRayReconstructionRoute()
{
    std::lock_guard<std::mutex> lock(g_nrMutex);
    return g_lastAfterRayReconstruction;
}
unsigned int ActivePassCount() { return g_activePasses.load(); }
const char* MultipassStatus() { return g_chainStatus.load(); }
const char* CadenceStatus() { return g_cadenceStatus.load(); }
bool CadenceAvailable() { return DlssNr_Cadence_Dx12::Available(); }
bool BufferIsLinearHdr() { return g_lastLinearHdr.load(); }

// HDR at present, for the menu. Written by the present hosts on their threads, read by the menu on its own.
std::mutex Dx12Internal::g_presentHdrStatusMutex;
std::chrono::steady_clock::time_point Dx12Internal::g_presentHdrAt {};
PresentColour Dx12Internal::g_presentHdrProfile = PresentColour::Declined;
const char* Dx12Internal::g_presentHdrReason = nullptr;
bool Dx12Internal::g_presentHdrConverted = false;
float Dx12Internal::g_presentHdrWhiteNits = 203.0f;
bool Dx12Internal::g_presentHdrWhiteFromDisplay = false;

void NotePresentHdr(PresentColour profile, const char* reason, bool converted, float whiteNits, bool whiteFromDisplay)
{
    std::lock_guard<std::mutex> lock(g_presentHdrStatusMutex);
    g_presentHdrAt = std::chrono::steady_clock::now();
    g_presentHdrProfile = profile;
    g_presentHdrReason = reason;
    g_presentHdrConverted = converted;
    g_presentHdrWhiteNits = whiteNits;
    g_presentHdrWhiteFromDisplay = whiteFromDisplay;
}

PresentHdrStatus GetPresentHdrStatus()
{
    PresentHdrStatus status;
    status.available = DlssNr_PresentHdr_Dx12::Available();

    std::lock_guard<std::mutex> lock(g_presentHdrStatusMutex);
    if (g_presentHdrAt.time_since_epoch().count() == 0)
        return status;

    const auto age = std::chrono::steady_clock::now() - g_presentHdrAt;
    status.seen = age < std::chrono::seconds(2);
    status.converted = status.seen && g_presentHdrConverted && age < std::chrono::seconds(1);
    status.whiteNits = g_presentHdrWhiteNits;
    status.whiteFromDisplay = g_presentHdrWhiteFromDisplay;
    status.profile = g_presentHdrProfile;
    status.reason = g_presentHdrReason;
    return status;
}

PeripheryState PeripheryStatus()
{
    std::lock_guard<std::mutex> lock(g_peripheryStatusMutex);
    return g_peripheryStatus;
}

void RequestCapture(unsigned int frames)
{
    ClearCaptureDirectory();
    g_capture.request(frames);
}

bool CaptureInProgress() { return g_capture.isActive(); }

// Only asks. The run's state is reset by Dispatch when it starts the run, under g_nrMutex: resetting slots
// here could hand a slot whose readback is still in flight to the next sample.
void StartMeasureDetail()
{
    std::lock_guard<std::mutex> lock(g_detailMeasure.mutex);
    if (g_detailMeasure.running || g_detailMeasure.cancelRequested)
        return;
    g_detailMeasure.wanted = true;
    g_detailMeasure.status.running = true;
    g_detailMeasure.status.initFailed = false;
    g_detailMeasure.status.endReason = nullptr;
    g_detailMeasure.status.progress = 0.0f;
    g_detailMeasure.status.samples = 0;
}

void CancelMeasureDetail()
{
    std::lock_guard<std::mutex> lock(g_detailMeasure.mutex);
    g_detailMeasure.wanted = false;
    g_detailMeasure.cancelRequested = true;
    g_detailMeasure.status.running = false;
}

bool DetailMeasureAvailable()
{
    return DlssNr_DetailStats_Dx12::Available() && !Config::Instance()->DlssNrUseProxy.value_or_default() &&
           Config::Instance()->DlssNrEnabled.value_or_default() && IsRunning();
}

DetailMeasureStatus GetDetailMeasureStatus()
{
    std::lock_guard<std::mutex> lock(g_detailMeasure.mutex);
    return g_detailMeasure.status;
}

bool PulseAvailable() { return DlssNr_Pulse_Dx12::Available(); }

PulseStatus GetPulseStatus()
{
    std::lock_guard<std::mutex> lock(g_pulse.statusMutex);
    return g_pulse.status;
}

void Shutdown()
{
    // Before the locks: the transport takes its own lock and then g_nrMutex when it captures, so it is never asked
    // to while g_nrMutex is held.
    ShutdownNativeDx11Guides();

    std::lock_guard<std::mutex> preLock(g_preMutex);
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    const bool pendingClones = std::any_of(
        std::begin(g_presentGuideClones), std::end(g_presentGuideClones),
        [](const auto& clone)
        {
            return clone.depth != nullptr && ((clone.tracked && !DlssNr::Submission::Ready(clone.writeUsage)) ||
                                              (clone.readTracked && !DlssNr::Submission::Ready(clone.readUsage)));
        });

    if (g_tracked)
    {
        TickNrRetired(true);
        const bool pendingRetired =
            std::any_of(g_nrRetired.begin(), g_nrRetired.end(), [](const NrRetired& retired)
                        { return retired.tracked && !DlssNr::Submission::Ready(retired.usage); });
        if (!DlssNr::Submission::Ready(g_usage) || pendingRetired || pendingClones)
        {
            LOG_INFO("DLSS-NR shutdown: retained all objects because tracked recordings are unsealed, pending or "
                     "unknown; no unsafe release");
            return;
        }
    }
    else if (pendingClones)
    {
        LOG_INFO("DLSS-NR shutdown: retained guide clones because tracked present recordings are unsealed or pending; "
                 "no unsafe release");
        return;
    }

    std::vector<NrEnlarger*> enlargers;

    for (auto& r : g_nrRetired)
    {
        delete r.scaler;
        delete r.stabilizer;
        delete r.uiMask;
        delete r.cadence;
        delete r.detailStats;
        delete r.pulse;
        delete r.motionProbe;
        if (r.feature != nullptr && g_nr.release != nullptr)
            g_nr.release(r.feature);

        if (r.resource != nullptr)
            r.resource->Release();

        if (r.enlarger != nullptr)
            enlargers.push_back(r.enlarger);
    }

    g_nrRetired.clear();

    // Transfer 2's private DLSS, retired and live, once the list is settled (see TickNrRetired). Its
    // destructor makes no NGX call while the process is exiting.
    enlargers.push_back(g_enlarge.live);
    g_enlarge.live = nullptr;
    for (NrEnlarger* enlarger : enlargers)
        delete enlarger;

    // Clear the settling state with the feature it was settling for. Left behind, the next
    // generation inherits a stability it never earned and rebuilds on its first frame -- the exact
    // churn the gate exists to prevent. This says nothing about device replacement being safe, and
    // does not authorise releasing anything still pending.
    g_postExtent.Reset();

    if (g_nr.feature != nullptr && g_nr.release != nullptr)
        g_nr.release(g_nr.feature);

    g_nr.feature = nullptr;

    for (void*& f : g_nr.passFeature)
    {
        if (f != nullptr && g_nr.release != nullptr)
            g_nr.release(f);

        f = nullptr;
    }

    if (g_nr.passPing)
    {
        g_nr.passPing->Release();
        g_nr.passPing = nullptr;
    }

    if (g_pre.scratch != nullptr)
    {
        g_pre.scratch->Release();
        g_pre.scratch = nullptr;
    }

    if (g_nr.output != nullptr)
    {
        g_nr.output->Release();
        g_nr.output = nullptr;
    }

    if (g_nr.colorCopy != nullptr)
    {
        g_nr.colorCopy->Release();
        g_nr.colorCopy = nullptr;
    }

    if (g_nr.hdrCopy != nullptr)
    {
        g_nr.hdrCopy->Release();
        g_nr.hdrCopy = nullptr;
    }

    if (g_nr.colorSmall != nullptr)
    {
        g_nr.colorSmall->Release();
        g_nr.colorSmall = nullptr;
    }

    if (g_nr.superUp != nullptr)
    {
        delete g_nr.superUp;
        g_nr.superUp = nullptr;
    }

    if (g_nr.depthMatched != nullptr)
    {
        g_nr.depthMatched->Release();
        g_nr.depthMatched = nullptr;
    }

    if (g_nr.motionMatched != nullptr)
    {
        g_nr.motionMatched->Release();
        g_nr.motionMatched = nullptr;
    }

    if (g_nr.guideMatch != nullptr)
    {
        delete g_nr.guideMatch;
        g_nr.guideMatch = nullptr;
    }

    ShutdownMotionProbe();

    for (ID3D12Resource** r : { &g_nr.unpackedProxy, &g_nr.unpackedAnswer })
    {
        if (*r != nullptr)
        {
            (*r)->Release();
            *r = nullptr;
        }
    }

    if (g_nr.periphery != nullptr)
    {
        delete g_nr.periphery;
        g_nr.periphery = nullptr;
    }

    if (g_detailMeasure.pass != nullptr)
    {
        delete g_detailMeasure.pass;
        g_detailMeasure.pass = nullptr;
    }

    for (auto& outRes : g_detailMeasure.outputs)
    {
        if (outRes != nullptr)
        {
            outRes->Release();
            outRes = nullptr;
        }
    }

    for (auto& inRes : g_detailMeasure.inputs)
    {
        if (inRes != nullptr)
        {
            inRes->Release();
            inRes = nullptr;
        }
    }
    g_detailMeasure.outputReadable[0] = g_detailMeasure.outputReadable[1] = false;
    g_detailMeasure.inputReadable[0] = g_detailMeasure.inputReadable[1] = false;

    // The pulse metric's pass; the readings go with it, and the next session starts measuring afresh.
    if (g_pulse.pass != nullptr)
    {
        delete g_pulse.pass;
        g_pulse.pass = nullptr;
    }
    for (auto& slot : g_pulse.slots)
        slot = {};
    g_pulse.next = 0;
    g_pulse.active = false;
    g_pulse.shape.reset();
    g_pulse.tracker.Reset();
    g_pulse.logGate.Reset();
    PublishPulse(false, false);

    if (g_nr.superDown != nullptr)
    {
        delete g_nr.superDown;
        g_nr.superDown = nullptr;
    }

    if (g_nr.outputNative != nullptr)
    {
        g_nr.outputNative->Release();
        g_nr.outputNative = nullptr;
    }

    for (ID3D12Resource** r : { &g_nr.alt.output, &g_nr.alt.colorCopy, &g_nr.alt.hdrCopy, &g_nr.alt.colorSmall,
                                &g_nr.alt.outputNative, &g_nr.alt.passPing })
    {
        if (*r != nullptr)
            (*r)->Release();
    }
    g_nr.alt = {};

    if (g_nr.heldColor != nullptr)
    {
        g_nr.heldColor->Release();
        g_nr.heldColor = nullptr;
    }
    g_nr.heldActive = false;

    if (g_nr.stabilizer != nullptr)
    {
        delete g_nr.stabilizer;
        g_nr.stabilizer = nullptr;
    }

    if (g_nr.uiMask != nullptr)
    {
        delete g_nr.uiMask;
        g_nr.uiMask = nullptr;
    }

    // The edit it held was made by the feature released above.
    if (g_nr.cadence != nullptr)
    {
        delete g_nr.cadence;
        g_nr.cadence = nullptr;
    }
    g_cadence.Drop();

    if (g_nr.meter != nullptr)
    {
        g_nr.meter->Release();
        g_nr.meter = nullptr;
    }

    if (g_nr.calib != nullptr)
    {
        g_nr.calib->Release();
        g_nr.calib = nullptr;
    }

    for (auto& r : g_nr.calibReadback)
    {
        if (r != nullptr)
        {
            r->Release();
            r = nullptr;
        }
    }

    g_nr.calibFrames = 0;
    g_nr.calibCount = 0;
    g_nr.calibSuggestion = 0.0f;
    g_nr.calibSteadiness = 0.0f;
    g_nr.calibUsable = false;
    g_nr.calibWhy = "measuring...";

    for (auto& rb : g_nr.meterReadback)
    {
        if (rb != nullptr)
        {
            rb->Release();
            rb = nullptr;
        }
    }

    // The slots these flags describe have just been released, so nothing may vouch for what the next
    // buffers happen to contain. gameExposure is deliberately NOT cleared here: a recreate is a
    // transition within the same scene, and dropping to the slider for a few frames would be the
    // flicker the held value exists to prevent. The user switching the option off is the case where
    // the held value has to go, and that is handled at the edge in Dispatch.
    for (bool& valid : g_nr.meterExposureValid)
        valid = false;

    g_nr.meterFrames = 0;
    g_nr.exposureLatch.Reset();

    if (g_nr.depthClone != nullptr)
    {
        g_nr.depthClone->Release();
        g_nr.depthClone = nullptr;
    }

    if (g_nr.motionClone != nullptr)
    {
        g_nr.motionClone->Release();
        g_nr.motionClone = nullptr;
    }

    g_capture.release();
    PairCapture::Shutdown();
    GpuTiming::SetEnabled(false);

    g_compose.reset();

    // The present route's motion estimator, once every present list that may read it has finished.
    if (g_presentMotion != nullptr)
    {
        if (WaitPresentListsIdle(2000))
            g_presentMotion->Release();
        else
            LOG_WARN("DLSS-NR shutdown: synthesized motion retained, a present list had not finished");
    }

    ResetPresentList();
    ReleasePresentTemporal();
    if (g_bbCopy != nullptr)
    {
        g_bbCopy->Release();
        g_bbCopy = nullptr;
    }

    // The HDR conversion's textures, retired ones included, and its pass, with the backbuffer copy they were made
    // beside.
    if (g_hdrWork != nullptr)
    {
        g_hdrWork->Release();
        g_hdrWork = nullptr;
    }
    if (g_hdrOut != nullptr)
    {
        g_hdrOut->Release();
        g_hdrOut = nullptr;
    }
    for (const auto& retired : g_hdrRetired)
        retired.resource->Release();
    g_hdrRetired.clear();
    delete g_presentHdr;
    g_presentHdr = nullptr;
    g_hdrWidth = g_hdrHeight = 0;
    g_hdrFormat = DXGI_FORMAT_UNKNOWN;
    g_hdrFailedWidth = g_hdrFailedHeight = 0;
    g_hdrFailedFormat = DXGI_FORMAT_UNKNOWN;
}
} // namespace DlssNr
