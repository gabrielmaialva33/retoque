#pragma once

// Private renderer state contract. Definitions stay in DlssNr_Dx12.cpp, in their original order.
// Never include this from the public feature API, and never define storage in this header.
// Locking and resource ownership are unchanged; see docs/dlssnr/design/dx12-internal.md.
#include "DlssNr_Dx12.h"
#include <dlssnr/DlssNr.h>
#include <dlssnr/DlssNr_Capture.h>
#include <dlssnr/DlssNr_Exposure.h>
#include <dlssnr/DlssNr_Submission.h>
#include <dlssnr/DlssNr_Cadence.h>
#include <dlssnr/DlssNr_CadenceAdaptive.h>
#include <dlssnr/DlssNr_DetailStats.h>
#include <dlssnr/DlssNr_Pulse.h>
#include <dlssnr/DlssNr_PresentGuideRing.h>
#include <dlssnr/DlssNr_PresentGuideIdentity.h>
#include <dlssnr/DlssNr_Enlarge.h>
#include "DlssNr_DetailStats_Dx12.h"
#include "DlssNr_Pulse_Dx12.h"
#include <Config.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class OS_Dx12;
class DlssNr_Stabilizer_Dx12;
class DlssNr_UiMask_Dx12;
class DlssNr_Cadence_Dx12;
class DlssNr_GuideMatch_Dx12;
class DlssNr_Periphery_Dx12;
class DlssNr_PresentHdr_Dx12;

namespace DlssNr::Dx12Internal
{
struct NrEnlarger; // Complete definition and destructor remain beside PrivateSrApi in the owner TU.

using PFN_NrCreate = void*(__cdecl*) (const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*,
                                      unsigned int, unsigned int, int, float, int, float, float, float, int, int);
using PFN_NrEvaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*,
                                     ID3D12Resource*, ID3D12Resource*, unsigned int, unsigned int, unsigned int,
                                     unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
                                     unsigned int, int, int, float, int, float, float, float, int, float, float);
using PFN_NrRelease = void(__cdecl*)(void*);
using PFN_NrSetExtras = void(__cdecl*)(void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned int,
                                       unsigned int, unsigned int, unsigned int);
using PFN_NrSetFloatSlot = void(__cdecl*)(int);
using PFN_NrProbeFloat = void(__cdecl*)(void*, const char*, float, int);

struct NrState
{
    HMODULE forwarder = nullptr;
    // [DlssNr] ModelLoader=direct: the function pointers below point into DlssNr_DirectRuntime rather
    // than into the forwarder, and forwarder stays null.
    bool directRuntime = false;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    PFN_NrSetExtras setExtras = nullptr;
    PFN_NrSetFloatSlot setFloatSlot = nullptr;
    PFN_NrProbeFloat probeFloat = nullptr;
    bool floatSlotKnown = false;

    // The scaling-ratio probe, resolved alongside the other forwarder entry points.
    int (*queryRatio)(const wchar_t*, void*, unsigned int, float*) = nullptr;
    const int* lastRatioStage = nullptr;
    int* lastInit = nullptr;
    int* lastCreate = nullptr;

    // The forwarder's record of a fault caught inside the model. Null on a forwarder that predates it.
    int (*faultState)(unsigned long*, void**) = nullptr;

    NVSDK_NGX_Parameter* capabilityParams = nullptr;
    void* feature = nullptr;

    // A feature per extra pass, each with its own temporal history.
    //
    // One feature run three times in a frame is told three frames passed with nothing moving between
    // them, so its history fights every pass after the first -- which is what "loses detail on later
    // passes" was. Separate features each see one frame per frame, which is the contract they were
    // built for.
    //
    // It is also the only reading that fits the one clue we have about how this is done elsewhere:
    // that implementation's memory grows with the pass count, and reusing a single feature cannot do
    // that. A feature apiece can, because each carries its own history.
    //
    // Indexed by pass, so [0] is unused and the first extra pass is [1]. Wasting one pointer keeps
    // every index here equal to the pass number it belongs to.
    void* passFeature[3] = {};
    ID3D12Resource* passPing = nullptr;
    DlssNrResolvedPassSettings passBuilt[3] {};
    bool passReset[3] { true, true, true };
    bool passFailed[3] {};
    DlssNr::Submission::Usage creation[3];
    uint64_t creationFrame[3] {};
    uint64_t measuredModelBytes = 0;

    // The model cannot read and write one resource, so the frame is staged through these.
    ID3D12Resource* colorCopy = nullptr;
    ID3D12Resource* output = nullptr;

    // The frame as the upscaler wrote it. The resolve adds the model's edit to this rather than
    // reconstructing it by inverting the tone curve, which is what turned every light in the frame into
    // a string of coloured cells.
    ID3D12Resource* hdrCopy = nullptr;

    // The frame shrunk for the model, when it is working below full resolution.
    ID3D12Resource* colorSmall = nullptr;

    // Supersampling (working scale > 1): the Output Scaling upsampler used to enlarge the proxy to the
    // model's larger-than-native working size with a real filter instead of the box minifier. Created
    // lazily on the first super-native frame, released in Shutdown; sizes from the resources each call,
    // so a resolution change needs no rebuild.
    OS_Dx12* superUp = nullptr;

    // Supersampling down-leg: the native-sized buffer the Nx model answer is averaged into, and the
    // downscaler that does it. With superUp this lands the super-native answer at native for a 1:1
    // composite (no aliased minify). nrScaler is the filter both were built with, so a changed
    // DlssNrScalingDownscaler rebuilds them.
    ID3D12Resource* outputNative = nullptr;
    OS_Dx12* superDown = nullptr;
    Scaler nrScaler = Scaler::Count;

    // The format-bound surfaces of the colour format the frame is not in right now, kept aside for a game
    // that alternates between two (FF7 Rebirth: R11G11B10 in play, RGBA16F on cutscene camera cuts) and
    // swapped back when the format returns at the same extent. See ReleaseSurfacesIfFormatChanged.
    struct AltSurfaces
    {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int workWidth = 0;
        unsigned int workHeight = 0;
        ID3D12Resource* output = nullptr;
        ID3D12Resource* colorCopy = nullptr;
        ID3D12Resource* hdrCopy = nullptr;
        ID3D12Resource* colorSmall = nullptr;
        ID3D12Resource* outputNative = nullptr;
        ID3D12Resource* passPing = nullptr;
    } alt;

    // Frame hold (docs/dlssnr/design/frame-hold.md): a persistent copy of the output taken on hold-on and restored
    // over the live output before the encode reads it while held, so a setting change re-renders the
    // same frame. heldWhitePoint is the snapshot used while held -- measurement is suspended.
    ID3D12Resource* heldColor = nullptr;
    bool heldActive = false;
    unsigned int heldWidth = 0;
    unsigned int heldHeight = 0;
    DXGI_FORMAT heldFormat = DXGI_FORMAT_UNKNOWN;
    float heldWhitePoint = 1.0f;

    // The output stabilizer (docs/dlssnr/design/output-stabilizer.md). Built for one output size and format, parked
    // like the downscalers when that changes or when it is switched off. stabilizerLast is when it last
    // ran, so a gap -- NR toggled, the stabilizer toggled, a stretch on the before-upscale stage --
    // drops history that no longer describes the screen.
    DlssNr_Stabilizer_Dx12* stabilizer = nullptr;
    std::chrono::steady_clock::time_point stabilizerLast {};

    // The static-overlay mask for UI protection (docs/dlssnr/design/hud-protection.md). Built for one working size,
    // parked like the stabilizer when that changes or when it is switched off. uiMaskLast is when it last
    // ran, so a gap drops a previous frame that no longer describes the screen.
    DlssNr_UiMask_Dx12* uiMask = nullptr;
    std::chrono::steady_clock::time_point uiMaskLast {};

    // Model cadence (docs/dlssnr/design/model-cadence.md): the surfaces that carry the model's last edit onto the
    // frames it does not run on. Built for one working size, parked like the stabilizer when that changes or when the
    // cadence is switched off or cannot run on the route. featureGeneration counts feature creations, so a
    // rebuilt model never inherits an edit made by the one before it; cadencePasses is what the last model
    // frame completed, which the menu keeps showing through carried frames.
    DlssNr_Cadence_Dx12* cadence = nullptr;
    uint64_t featureGeneration = 0;
    unsigned int cadencePasses = 0;

    unsigned int workWidth = 0;
    unsigned int workHeight = 0;

    // The white point meter.
    //
    // A 64x64 grid of tile luminances, copied to a readback buffer and looked at a few frames later.
    // Four buffers deep rather than one: the copy is recorded into the game's own command list and
    // there is no fence here to wait on, so the only thing making a read safe is that the frame it
    // came from is long retired. Three frames of distance is what the meter this replaces used.
    //
    // A stale read costs a slightly wrong float that the average below absorbs. A read of a buffer
    // still being written would cost the same, which is why the value is smoothed rather than used
    // raw.
    ID3D12Resource* meter = nullptr;
    ID3D12Resource* meterReadback[4] = {};

    // The calibration grid: what scale the game's buffer is on, measured from the untouched copy.
    // Its own surface and ring rather than sharing the meter's, because the two run at different
    // sizes -- the meter fetches one texel and this reads the whole frame.
    ID3D12Resource* calib = nullptr;
    ID3D12Resource* calibReadback[4] = {};
    unsigned long long calibFrames = 0;

    // The last few answers, so the menu can say how settled the number is. A suggestion taken during
    // a fade or a loading screen is worth less than one taken while standing still, and the spread
    // across recent frames is what tells them apart.
    static constexpr unsigned int kCalibHistory = 32;
    float calibHistory[kCalibHistory] = {};
    unsigned int calibCount = 0;
    float calibSuggestion = 0.0f;
    float calibSteadiness = 0.0f;
    bool calibUsable = false;
    const char* calibWhy = "measuring...";
    bool calibPassthrough = false;

    // Whether the frame that filled each readback slot actually had an exposure texture bound.
    //
    // The meter writes tile 0 from whatever sits in the exposure slot, and DispatchPass substitutes
    // the source picture when nothing is bound -- so without this the "exposure" read back is the red
    // channel of the frame's top-left pixel. In Cyberpunk, which supplies no exposure texture, that
    // pixel is scene content: it moved by up to 272x between consecutive frames and drove the white
    // point from 0.18 to 74. That is the whole frame flashing in luminance.
    //
    // The grid is read three frames after it is written, so the flag has to travel with the slot
    // rather than being asked of the current frame.
    bool meterExposureValid[4] = {};
    unsigned int meterSlot = 0;
    unsigned long long meterFrames = 0;

    // Whether the setting was on last frame, so the off->on edge can be caught.
    //
    // Deliberately the SETTING and not `wantExposure`: the texture itself comes and goes between
    // frames and holding the last good value across those gaps is the whole point of the field below.
    // Only the user turning the option back on means "anything held is from an unknown time ago".
    bool exposureSettingWasOn = false;

    // The game's exposure, as last read back, and the pre-exposure that goes with it. Held rather
    // than defaulted: the texture comes and goes between frames and a fallback to 1.0 on the gaps
    // would be a flicker source.
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;
    DlssNr::Exposure::PlausibilityLatch exposureLatch;

    // What the game OFFERS, as opposed to what has been read. Recorded from the parameter block every
    // frame whether or not the setting is on, and deliberately so: the menu has to be able to answer
    // "would this do anything here?" before the user turns it on, and reading a pointer for null costs
    // nothing. Whether it was ever offered is kept separately from whether it was offered this frame,
    // because games drop it on transitions -- GTA V dropped it three times in one session -- and one
    // absent frame is not the same answer as never.
    bool exposureOfferedNow = false;
    bool exposureEverOffered = false;
    unsigned long long exposureFrames = 0;

    // Cloned unconditionally when running at present, and only for typeless formats otherwise.
    ID3D12Resource* depthClone = nullptr;
    ID3D12Resource* motionClone = nullptr;

    // The constant-depth probe's surface. Separate from depthClone on purpose: it is defined by
    // never having been written, and sharing a surface with a mode that writes would destroy that.
    ID3D12Resource* depthConstant = nullptr;

    // Depth and motion at the model's working size, below the frame's (reduced-scale-guides.md). Sized
    // with the output, parked with it, and resting in UNORDERED_ACCESS between frames. guideMatch is the
    // pass that fills them, built on first use and only when its bytecode is in the build.
    ID3D12Resource* depthMatched = nullptr;
    ID3D12Resource* motionMatched = nullptr;
    DlssNr_GuideMatch_Dx12* guideMatch = nullptr;

    // Peripheral compression (peripheral-compression.md). While it runs, the working size above is the
    // packed extent and depthMatched/motionMatched hold the packed guides. periphery is the pass that packs
    // and unpacks, built on first use, only when its bytecode is in the build, and kept until shutdown like
    // guideMatch. The unpacked pair is the packed proxy and answer brought back onto the uniform grid for the
    // resolve: sized to that grid, resting in UNORDERED_ACCESS, parked when the grid moves or the feature
    // stops. peripheryFailed latches a pass that could not be built or run; Retry clears it.
    DlssNr_Periphery_Dx12* periphery = nullptr;
    ID3D12Resource* unpackedProxy = nullptr;
    ID3D12Resource* unpackedAnswer = nullptr;
    bool peripheryFailed = false;

    unsigned int width = 0;
    unsigned int height = 0;
    bool afterRayReconstruction = false;
    bool reset = true;

    // Dimensions of the guides as the upscaler handed them over, kept for the present path, which runs
    // long after that call has returned.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;

    // How the game encodes its guides, as the game itself reports it. Captured with the guides, since
    // the finished-frame path runs long after the upscaler's call has returned.
    bool guideDepthInverted = false;
    float guideMvScaleX = 1.0f;
    float guideMvScaleY = 1.0f;

    // The values the live feature was created with, and when a difference from them was first seen.
    unsigned int builtPreset = 0;
    float builtIntensity = 0.0f;
    unsigned int builtStyle = 0;
    float builtLocalStructure = 0.0f;
    float builtLocalTone = 0.0f;
    float builtSkinStructure = 0.0f;
    bool builtAutoMask = false;
    unsigned long long settledAt = 0;

    // Once something fails there is no recovering it mid-session, and retrying every frame turns a
    // failure into a crash. It stays off and says why.
    bool failed = false;
    const char* reason = "";

    // A size the model said no to, which is a different thing from a failure.
    //
    // FAIL_InvalidParameter answers the request, not the session: the model will take a smaller one.
    // Keeping the size rather than a flag means the refusal expires on its own -- the next frame at
    // any other extent is not the refused one and simply runs.
    bool rejectedWork = false;
    unsigned int rejectedWorkWidth = 0;
    unsigned int rejectedWorkHeight = 0;
};

struct PreUpscaleState
{
    ID3D12Resource* scratch = nullptr;
    unsigned int width = 0;
    unsigned int height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct NrRetired
{
    void* feature = nullptr;
    ID3D12Resource* resource = nullptr;
    int framesLeft = 32;
    bool tracked = false;
    DlssNr::Submission::Usage usage;
    // Tracked objects (chain-in-flight.md): freed once every epoch in usage is finished AND framesLeft ticks
    // passed, which covers work NGX may keep on queues of its own that no fence of ours sees.
    uint64_t parkedAt = DlssNr::Submission::Clock();
    // Waiting only on epochs no fence will ever answer: retained for good unless they finish, and out of the cap.
    bool stuck = false;
    OS_Dx12* scaler = nullptr;
    DlssNr_Stabilizer_Dx12* stabilizer = nullptr;
    DlssNr_UiMask_Dx12* uiMask = nullptr;
    DlssNr_Cadence_Dx12* cadence = nullptr;
    NrEnlarger* enlarger = nullptr;
    DlssNr_DetailStats_Dx12* detailStats = nullptr;
    DlssNr_Pulse_Dx12* pulse = nullptr;
};

struct DetailMeasureSession
{
    std::mutex mutex;
    bool wanted = false;
    bool cancelRequested = false;
    bool running = false;
    static constexpr unsigned kTarget = 60;
    uint32_t currentCopy = 0;
    bool hasPreviousFrame = false;
    float frozenWhitePoint = 1.0f;

    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    DlssNr_DetailStats_Dx12* pass = nullptr;
    ID3D12Resource* outputs[2] = {};
    ID3D12Resource* inputs[2] = {};
    bool outputReadable[2] = {};
    bool inputReadable[2] = {};

    // A run ends at kTarget still frames, or after kMaxAttempts readbacks (moving frames are dropped), or when
    // no sample has completed for kStallEvaluates evaluates. Fewer than kMinKept still frames is no result.
    static constexpr unsigned kMaxAttempts = 120;
    static constexpr unsigned kMinKept = 30;
    static constexpr unsigned kStallEvaluates = 240;
    unsigned attempts = 0;
    unsigned evaluatesWithoutSample = 0;

    uint32_t slot = 0;
    DlssNr::Submission::Usage slotUsage[DLSSNR_DETAIL_STATS_SLOTS] {};
    bool slotPending[DLSSNR_DETAIL_STATS_SLOTS] {};

    DlssNr::DetailStats::Accumulator accumulator;
    DlssNr::DetailMeasureStatus status;
};

struct PulseSession
{
    // What changes the meaning of a step: the frame's size and format, the cadence as configured and its smoothing,
    // the residual limit and final luminance floor. A change starts the window over, so no step spans it.
    struct Shape
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;
        uint32_t cadence = 1;
        bool smoothing = false;
        float residualLimit = 0.0f;
        float minRatio = 0.0f;

        bool operator==(const Shape&) const = default;
    };

    struct Slot
    {
        bool pending = false;
        bool stale = false; // recorded before the shape changed: collected, never counted
        uint64_t seq = 0;
        int64_t ms = 0;
        DlssNr::Pulse::Kind kind = DlssNr::Pulse::Kind::Every;
        uint32_t cadence = 1;
        DlssNr::Submission::Usage usage {};
    };

    DlssNr_Pulse_Dx12* pass = nullptr;
    Slot slots[DLSSNR_PULSE_SLOTS] {};
    uint32_t next = 0;
    // Presents at which the pass was asked to run, whether it then ran, skipped or failed: a measurement's sequence
    // number. Counted whether or not the metric is on (one comparison a call), so the first measurement after it goes
    // on has a number to pair with.
    uint64_t entered = 0;
    uint64_t lastEntered = 0;
    uint64_t lastPresent = 0; // the present last measured
    bool active = false;
    bool saidFailure = false;
    std::optional<Shape> shape;
    DlssNr::Pulse::Tracker tracker;
    DlssNr::Pulse::LogGate logGate;

    std::mutex statusMutex;
    DlssNr::PulseStatus status;
};

struct EnlargeSession
{
    NrEnlarger* live = nullptr;
    bool failed = false;
    bool lastHold = false;
    std::atomic<uint32_t> transfer { DlssNr::Enlarge::kTransferMatched };
    std::atomic<uint32_t> why { static_cast<uint32_t>(DlssNr::Enlarge::Why::NotYet) };
};

struct PresentGuideClone
{
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    const void* deviceIdentity = nullptr;
    DlssNrFrameInfo frame {};
    DlssNr::Submission::Usage writeUsage {};        // accumulated for resource retirement / shutdown
    DlssNr::Submission::Usage captureWriteUsage {}; // single capture write usage for eligibility
    DlssNr::Submission::Usage readUsage {};         // accumulated for resource retirement / shutdown
    DlssNr::Submission::Usage presentReadUsage {};  // single present read usage for in-flight check
    bool tracked = false;
    bool readTracked = false;
    uint32_t width = 0;
    uint32_t height = 0;

    DXGI_FORMAT sourceDepthFormat = DXGI_FORMAT_UNKNOWN;
    uint64_t sourceDepthWidth = 0;
    uint32_t sourceDepthHeight = 0;
    D3D12_RESOURCE_FLAGS sourceDepthFlags = D3D12_RESOURCE_FLAG_NONE;

    DXGI_FORMAT sourceMotionFormat = DXGI_FORMAT_UNKNOWN;
    uint64_t sourceMotionWidth = 0;
    uint32_t sourceMotionHeight = 0;
    D3D12_RESOURCE_FLAGS sourceMotionFlags = D3D12_RESOURCE_FLAG_NONE;
};

struct BridgeGuideRead
{
    bool valid = false;
    size_t slot = 0;
    ID3D12GraphicsCommandList* list = nullptr; // compared and passed to Abandon, not held
    DlssNr::Submission::Usage usage {};
    DlssNr::Submission::Usage presentUsage {};
    bool tracked = false;
};

struct PresentGuideIdentityState
{
    DlssNr::PresentGuideIdentity::Tracker tracker;
    DlssNr::PresentGuideIdentity::Consumer present { "D3D12 present" };
    DlssNr::PresentGuideIdentity::Consumer bridge { "D3D11 bridge" };
    DlssNr::PresentGuideIdentity::SourceHistory presentSource;
    DlssNr::PresentGuideIdentity::SourceHistory bridgeSource;
    DlssNr::PresentGuideIdentity::StatusWindow status;
};

struct PresentList
{
    static constexpr unsigned int kSlots = 4;
    ID3D12CommandAllocator* allocator[kSlots] = {};
    ID3D12GraphicsCommandList* list[kSlots] = {};
    UINT64 fenceValue[kSlots] = {};
    bool dirty[kSlots] = {};
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
};

struct PresentHdrRetired
{
    ID3D12Resource* resource = nullptr;
    UINT64 fenceValue = 0;
};

// One definition of each object, in the owner TU; no inline or header-static state.
extern std::mutex g_coverageMutex;
extern DlssNr::Detail::StageCoverage g_coverage[4];
extern DlssNr::Detail::CoverageTotals g_coverageLogged[4];
extern NrState g_nr;
extern std::unique_ptr<DlssNr_Dx12> g_compose;
extern PreUpscaleState g_pre;
extern std::mutex g_preMutex;
extern DlssNr::Detail::StableExtent g_preExtent;
extern DlssNr::Detail::CreationFrameGate g_preCreation;
extern ID3D12Device* g_preDevice;
extern bool g_preAllocationFailed;
extern bool g_lastBeforeUpscale;
extern bool g_lastAfterRayReconstruction;
extern std::atomic<const char*> g_preStatus;
extern uint64_t g_timingEvaluations;
extern capture::FrameCapture g_capture;
extern unsigned long long g_frames;
extern unsigned long long g_captureWriteAtFrame;
extern std::filesystem::path g_dllDir;
extern bool g_tracked;
extern bool g_legacyRecorded;
extern DlssNr::Submission::Usage g_usage;
extern bool g_haveTrackedUsage;
extern DlssNr::Chain::RecordingGate g_recordings;
extern DlssNr::Chain::Schedule g_chainSchedule;
extern DlssNr::Detail::StableExtent g_chainExtent;
extern DlssNr::Detail::StableExtent g_postExtent;
extern std::atomic<unsigned int> g_activePasses;
extern std::atomic<const char*> g_chainStatus;
extern std::atomic<bool> g_resetOnReturn;
extern std::atomic<bool> g_lastLinearHdr;
extern DlssNr::Cadence::Scheduler g_cadence;
extern DlssNr::Cadence::Adaptive::Controller g_cadenceAdaptive;
extern uint64_t g_cadenceEvaluations;
extern std::atomic<const char*> g_cadenceStatus;
extern uint64_t g_nrTicks;
extern std::vector<NrRetired> g_nrRetired;
extern DetailMeasureSession g_detailMeasure;
extern PulseSession g_pulse;
extern uint64_t g_nrScratchGeneration;
extern std::mutex g_nrMutex;
extern std::mutex g_peripheryStatusMutex;
extern DlssNr::PeripheryState g_peripheryStatus;
extern EnlargeSession g_enlarge;
extern PresentGuideClone g_presentGuideClones[DlssNr::PresentGuides::kRingSize];
extern BridgeGuideRead g_bridgeRead;
extern DlssNr::PresentGuides::Ring g_presentGuideRing;
extern std::atomic<bool> g_temporalValid;
extern std::atomic<long long> g_presentHookAtMs;
extern std::atomic<long long> g_boundaryAtMs;
extern std::atomic<long long> g_presentRouteAtMs;
extern unsigned long long g_renderSeq;
extern unsigned long long g_nrLastEnhancedSeq;
extern unsigned long long g_nrLastEnhancedRecordedSeq;
extern unsigned long long g_presentRuns;
extern unsigned long long g_presentSkips;
extern ID3D12Resource* g_lastEnhancedBb;
extern unsigned int g_lastEnhancedBbIdx;
extern unsigned int g_sameBbStreak;
extern unsigned int g_maxSameBbStreak;
extern PresentGuideIdentityState g_guideIdentity;
extern std::atomic<uint64_t> g_guideStatus;
extern std::atomic<long long> g_guideStatusAtMs;
extern std::atomic<uint8_t> g_guideFallback;
extern std::atomic<uint64_t> g_bridgePresentedExtent;
extern PresentList g_presentList;
extern UINT64 g_presentFenceValue;
extern unsigned long long g_presentFlip;
extern unsigned long long g_lastFgFlip;
extern unsigned long long g_upscaleRenders;
extern std::atomic<bool> g_upscaleApplied;
extern std::atomic<const char*> g_upscaleReason;
extern std::atomic<unsigned long long> g_passesRecorded;
extern std::atomic<bool> g_sourceIsPresent;
extern ID3D12Resource* g_bbCopy;
extern unsigned int g_bbCopyWidth;
extern unsigned int g_bbCopyHeight;
extern DXGI_FORMAT g_bbCopyFormat;
extern DlssNr::SynthMotionGuide* g_presentMotion;
extern DlssNr_PresentHdr_Dx12* g_presentHdr;
extern std::vector<PresentHdrRetired> g_hdrRetired;
extern ID3D12Resource* g_hdrWork;
extern ID3D12Resource* g_hdrOut;
extern unsigned int g_hdrWidth;
extern unsigned int g_hdrHeight;
extern DXGI_FORMAT g_hdrFormat;
extern unsigned int g_hdrFailedWidth;
extern unsigned int g_hdrFailedHeight;
extern DXGI_FORMAT g_hdrFailedFormat;
extern std::mutex g_presentHdrStatusMutex;
extern std::chrono::steady_clock::time_point g_presentHdrAt;
extern PresentColour g_presentHdrProfile;
extern const char* g_presentHdrReason;
extern bool g_presentHdrConverted;
extern float g_presentHdrWhiteNits;
extern bool g_presentHdrWhiteFromDisplay;
} // namespace DlssNr::Dx12Internal

// Functions one renderer unit defines and another calls, with the unit that defines them. Public entry points keep
// their declarations in DlssNrFeature_Dx12.h; these are the renderer's own.
namespace DlssNr
{
// DlssNr_Dx12_Inputs.cpp
void ReportSpatialContract(NVSDK_NGX_Parameter* params, ID3D12Resource* color, ID3D12Resource* output, bool before,
                           unsigned int inputWidth, unsigned int inputHeight);
DlssNrFrameInfo GatherFrame(NVSDK_NGX_Parameter* params);
} // namespace DlssNr
