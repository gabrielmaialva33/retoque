#include "pch.h"

// The renderer's diagnostics: the timing contract and metadata, comparison-capture triggers, the detail-measure and
// pulse-metric helpers, the exposure meter and calibration readbacks, and the once-per-reason skip report. Moved
// whole from DlssNr_Dx12.cpp (split step 6, docs/dlssnr/design/dx12-internal.md); bodies and function-local
// statics are unchanged. The state they read and write is still defined in DlssNr_Dx12.cpp.

#include "DlssNr_Dx12_Internal.h"
#include <dlssnr/DlssNr.h>
#include <dlssnr/DlssNr_Capture.h>
#include <dlssnr/DlssNr_PairCaptureDx12.h>
#include <dlssnr/DlssNr_Proxy.h>
#include <dlssnr/DlssNr_ExposureScan.h>
#include <dlssnr/DlssNr_Replay.h>
#include <dlssnr/DlssNr_ReplayClock.h>
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
#include "DlssNr_PresentHdr_Dx12.h"
#include <dlssnr/DlssNr_Pulse.h>
#include <dlssnr/DlssNr_PresentGuideRing.h>
#include <menu/menu_overlay_base.h>
#include <dlssnr/DlssNr_PresentGuideIdentity.h>
#include <dlssnr/DlssNr_ReplayRecorderDx12.h>

namespace DlssNr::Dx12Internal
{
// Timing identifies the loaded model file once per process. These file attributes are an audit
// label, not a content hash or proof that the file was unchanged while the process was running.
const std::string& TimingModelIdentity()
{
    static const std::string identity = []
    {
        wchar_t buffer[32768] {};
        const auto module = GetModuleHandleW(L"nvngx_dlssnr.dll");
        const auto count = module ? GetModuleFileNameW(module, buffer, _countof(buffer)) : 0;
        if (!count || count >= _countof(buffer))
            return std::string("loaded-model-unidentified");
        const std::filesystem::path path(buffer);
        std::error_code sizeError, timeError;
        const auto bytes = std::filesystem::file_size(path, sizeError);
        const auto modified = std::filesystem::last_write_time(path, timeError);
        version_t version {};
        const bool hasVersion = Util::GetFileVersion(path.wstring(), &version);
        const auto label = std::format("file-bytes={};mtime={};version={}.{}.{}.{}", sizeError ? 0 : bytes,
                                       timeError ? 0 : modified.time_since_epoch().count(), version.major,
                                       version.minor, version.patch, version.reserved);
        LOG_INFO("DLSS-NR timing model-file: path={} label={} size_known={} mtime_known={} version_known={} "
                 "identity=file-attributes-not-content-hash; hash the installed model externally before/after A/B",
                 path.string(), label, !sizeError, !timeError, hasVersion);
        return label;
    }();
    return identity;
}

// Every setting that shapes the picture, as text: the timing contract, and the settings in a comparison capture's
// provenance (docs/dlssnr/design/comparison-capture.md, section 4.4). One function for both, so the two cannot drift.
std::string TimingContract(const Config& cfg, const DlssNrPassSnapshot& passes, const char* stage, unsigned renderWidth,
                           unsigned renderHeight, unsigned width, unsigned height, unsigned workWidth,
                           unsigned workHeight, float workScale, bool linearHdr, unsigned format, unsigned cadence,
                           uint32_t effectiveTransfer, const char* enlarge)
{
    auto contract = std::format(
        "stage={} render={}x{} compose={}x{} model={}x{} scale={} passes={} individual={} "
        "apply={} hold={} white_source={} white_scale={} white_trim={} scan_trim={} scan_inverted={} "
        "detail={} colour={} max_ratio={} transfer={} reversible={} debug={} compare={} split={} zoom={} swap={} "
        "downscaler={} exposure_scan={} linear_hdr={} compose_format={}",
        stage, renderWidth, renderHeight, width, height, workWidth, workHeight, workScale, passes.Count,
        passes.Individual, cfg.DlssNrApplyModel.value_or_default(), cfg.DlssNrHoldFrame.value_or_default(),
        cfg.DlssNrWhitePointSource.value_or_default(), cfg.DlssNrWhitePointScale.value_or_default(),
        cfg.DlssNrWhitePointTrim.value_or_default(), cfg.DlssNrScanTrim.value_or_default(),
        cfg.DlssNrScanInverted.value_or_default(), cfg.DlssNrTransferStrength.value_or_default(),
        cfg.DlssNrColourStrength.value_or_default(), cfg.DlssNrMaxRatio.value_or_default(),
        cfg.DlssNrTransfer.value_or_default(), cfg.DlssNrReversibleMode.value_or_default(),
        cfg.DlssNrDebugView.value_or_default(), cfg.DlssNrCompare.value_or_default(),
        cfg.DlssNrCompareSplit.value_or_default(), cfg.DlssNrCompareZoom.value_or_default(),
        cfg.DlssNrCompareSwap.value_or_default(), static_cast<unsigned>(cfg.DlssNrScalingDownscaler.value_or_default()),
        cfg.DlssNrScanExposure.value_or_default(), linearHdr, format);
    if (const float minimum = DlssNr::MinimumRatio(cfg.DlssNrMinRatio.value_or_default()); minimum > 0.0f)
        contract += std::format(" min_ratio={}", minimum);
    // What the resolve is actually sent and whether Transfer 2's private DLSS ran (its reason token otherwise):
    // that DLSS lands in outside_model_ms, so a sample from its warm-up is not a sample of it.
    contract += std::format(" effective_transfer={} enlarge={}", effectiveTransfer, enlarge ? enlarge : "unknown");
    for (unsigned i = 0; i < passes.Count; ++i)
    {
        const auto& pass = passes.Settings[i];
        contract += std::format(" pass{}=[preset={},style={},intensity={},structure={},tone={},skin={},mask={}]", i + 1,
                                pass.Preset, pass.Style, pass.Intensity, pass.LocalStructure, pass.LocalTone,
                                pass.SkinStructure, pass.AutoMask);
    }
    // The cadence in effect is part of the configuration; whether this evaluation was a carried frame is not,
    // like the reset. Only named when it runs, so a contract with it off reads as it always did.
    if (cadence > 1)
        contract += std::format(" cadence={}", cadence);
    // Peripheral compression's layout, only while it is switched on, so the default contract keeps its text and
    // hash. Two centre bands at one packed extent cost the same pixels but are not the same measurement.
    if (cfg.DlssNrPeripheryCompression.value_or_default())
        contract += std::format(" periphery=[centre={},work={}]", cfg.DlssNrPeripheryCenter.value_or_default(),
                                cfg.DlssNrPeripheryWork.value_or_default());
    // Highlight recovery, only when the resolve is sent it (on, and the buffer is linear HDR), for the same reason:
    // it adds work per bright pixel, and off keeps the default contract's text and hash.
    if (linearHdr && cfg.DlssNrHighlightRecovery.value_or_default())
        contract += " highlight_recovery=1";
    return contract;
}

// Exact text comparison gives each configuration a session-local generation. Hash is only a compact
// lookup key; the INFO contract line carries the values and never claims cryptographic identity.
// Legacy scalar settings are not one atomic render transaction: keep configuration fixed during A/B.
DlssNr::GpuTiming::Metadata TimingMetadata(const Config& cfg, const DlssNrPassSnapshot& passes, uint64_t evaluation,
                                           uint64_t present, const char* stage, unsigned renderWidth,
                                           unsigned renderHeight, unsigned width, unsigned height, unsigned workWidth,
                                           unsigned workHeight, float workScale, bool reset, bool captureActive,
                                           bool linearHdr, unsigned format, unsigned cadence, bool carried,
                                           uint32_t effectiveTransfer, const char* enlarge)
{
    const auto contract = TimingContract(cfg, passes, stage, renderWidth, renderHeight, width, height, workWidth,
                                         workHeight, workScale, linearHdr, format, cadence, effectiveTransfer, enlarge);
    static std::string previous;
    static uint64_t generation = 0;
    static uint64_t hash = 0;
    if (previous != contract)
    {
        // An unchanged contract already has its hash; avoid rehashing the full text every evaluation.
        uint64_t updatedHash = 14695981039346656037ull;
        for (const unsigned char ch : contract)
            updatedHash = (updatedHash ^ ch) * 1099511628211ull;
        previous = contract;
        hash = updatedHash;
        ++generation;
        LOG_INFO("DLSS-NR timing contract generation={} hash={:016X}: {}", generation, hash, contract);
    }
    DlssNr::GpuTiming::Metadata metadata {};
    metadata.evaluationId = evaluation;
    metadata.presentId = present;
    metadata.settingsGeneration = generation;
    metadata.contractHash = hash;
    metadata.stage = stage;
    metadata.renderWidth = renderWidth;
    metadata.renderHeight = renderHeight;
    metadata.outputWidth = width;
    metadata.outputHeight = height;
    metadata.modelWidth = workWidth;
    metadata.modelHeight = workHeight;
    metadata.workingScale = workScale;
    metadata.requestedPasses = passes.Count;
    metadata.modelReset = reset;
    metadata.captureActive = captureActive;
    metadata.cadence = cadence > 1 ? cadence : 1;
    metadata.carried = carried;
    const auto& identity = TimingModelIdentity();
    std::snprintf(metadata.modelIdentity, sizeof(metadata.modelIdentity), "%s", identity.c_str());
    return metadata;
}

// Cleared once per run, so a session's captures are its own and nothing accumulates across launches.
void ClearCaptureDirectory()
{
    static bool cleared = false;

    if (cleared)
        return;

    cleared = true;

    std::error_code ec;
    const auto dir = Util::DllPath().remove_filename() / "dlssnr-capture";

    if (std::filesystem::exists(dir, ec))
    {
        std::filesystem::remove_all(dir, ec);

        if (ec)
            LOG_WARN("DLSS-NR could not clear {}: {}", dir.string(), ec.message());
    }
}

// Dropping a file named dlssnr-capture.trigger beside OptiScaler requests a capture, so a session can
// be asked for one from outside the game -- no alt-tab, no menu. Checked once a second, effectively.
//
// It asks for the comparison capture: one frame, read and written, as image files with provenance
// (docs/dlssnr/design/comparison-capture.md). A file whose content is the word "burst" asks for the raw
// eight-frame burst instead, which temporal research still uses.
void CheckCaptureTrigger()
{
    if ((g_frames % 60) != 0)
        return;

    std::error_code ec;
    const auto trigger = Util::DllPath().remove_filename() / "dlssnr-capture.trigger";

    if (std::filesystem::exists(trigger, ec))
    {
        char content[16] = {};
        if (std::FILE* f = _wfopen(trigger.wstring().c_str(), L"rb"))
        {
            std::fread(content, 1, sizeof(content) - 1, f);
            std::fclose(f);
        }
        std::filesystem::remove(trigger, ec);

        // Letters only, so a newline, spaces or an editor's byte-order mark around the word do not change it.
        std::string word(content);
        word.erase(
            std::remove_if(word.begin(), word.end(), [](unsigned char c) { return c >= 0x80 || std::isalpha(c) == 0; }),
            word.end());
        std::transform(word.begin(), word.end(), word.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (word == "burst")
        {
            DlssNr::RequestCapture(capture::kMaxFrames);
            LOG_INFO("DLSS-NR capture: the raw burst requested by trigger file");
        }
        else if (word == "replay")
        {
            // A replay clip (offline-replay-recorder.md): the next after-upscale evaluation starts it.
            const char* why = nullptr;
            if (!DlssNr::ReplayRecorder::Request(&why))
                LOG_INFO("DLSS-NR replay recorder: trigger file ignored: {}", why != nullptr ? why : "not taken");
        }
        else if (const char* why = nullptr; !DlssNr::RequestPairCapture(&why))
        {
            LOG_INFO("DLSS-NR capture: trigger file ignored: {}", why != nullptr ? why : "the request was not taken");
        }
    }
}

// The encoded mean is aimed here. Mid-grey rather than anything brighter: the model has to see both the
// shadow detail it might lift and the highlights it must not blow out.
constexpr float kTargetEncodedMean = 0.45f;

// Recomputes the white point from a measured mean. Inverting the encode for the white point that puts
// that mean at the target gives wp = mean * (1 - t^g) / t^g.
float WhitePointForMean(float meanLuma)
{
    const float encoded = powf(kTargetEncodedMean, 2.2f);
    const float ratio = encoded / (1.0f - encoded);
    const float wp = meanLuma / ratio;
    // A black frame between scenes would otherwise drive this to zero and divide the next frame by it.
    return wp < 0.01f ? 0.01f : (wp > 10000.0f ? 10000.0f : wp);
}

// Parks the pass and the frame copies through the retirement list. Caller holds g_nrMutex and the session mutex.
void ParkDetailMeasureLocked()
{
    ParkNrDetailStats(g_detailMeasure.pass);
    for (auto& outRes : g_detailMeasure.outputs)
        ParkNrResource(outRes);
    for (auto& inRes : g_detailMeasure.inputs)
        ParkNrResource(inRes);
    g_detailMeasure.outputReadable[0] = g_detailMeasure.outputReadable[1] = false;
    g_detailMeasure.inputReadable[0] = g_detailMeasure.inputReadable[1] = false;
    for (bool& p : g_detailMeasure.slotPending)
        p = false;
    g_detailMeasure.running = false;
}

void DetailMeasureCopy(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, ID3D12Resource*& copy, bool& readable,
                       ID3D12Resource* from, D3D12_RESOURCE_STATES fromState)
{
    if (copy != nullptr)
    {
        const auto have = copy->GetDesc();
        const auto want = from->GetDesc();
        if (have.Width != want.Width || have.Height != want.Height || have.Format != want.Format)
        {
            ParkNrResource(copy);
            readable = false;
        }
    }

    if (copy == nullptr)
    {
        D3D12_RESOURCE_DESC desc = from->GetDesc();
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        desc.Alignment = 0;
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&copy))))
            return;
        readable = false;
    }

    if (readable)
        Barrier(cmdList, copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    Barrier(cmdList, from, fromState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(copy, from);
    Barrier(cmdList, from, D3D12_RESOURCE_STATE_COPY_SOURCE, fromState);
    Barrier(cmdList, copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    readable = true;
}

// At the top of every Dispatch that got as far as reading the present counter. A second call in one present is the
// same present; where presents are not counted (the counter stays 0) every call is a new one.
void NotePulsePresent(uint64_t present)
{
    if (present == 0 || present != g_pulse.lastEntered)
        ++g_pulse.entered;
    g_pulse.lastEntered = present;
}

void PublishPulse(bool running, bool failed)
{
    std::lock_guard<std::mutex> lock(g_pulse.statusMutex);
    g_pulse.status.running = running;
    g_pulse.status.failed = failed;
    g_pulse.status.words = g_pulse.tracker.Latest().Words();
}

// Off: the pass is parked, the readings forgotten. Its slots' readbacks may still be in flight; they belong to the
// parked object and are freed with it.
static void StopPulseLocked()
{
    if (!g_pulse.active && g_pulse.pass == nullptr)
        return;

    ParkNrPulse(g_pulse.pass);
    for (auto& slot : g_pulse.slots)
        slot = {};
    g_pulse.next = 0;
    g_pulse.active = false;
    g_pulse.saidFailure = false;
    g_pulse.shape.reset();
    g_pulse.tracker.Reset();
    g_pulse.logGate.Reset();
    PublishPulse(false, false);
}

// Once per Dispatch, after everything that writes the target. Collects the readbacks that completed, oldest first;
// starts over if what is measured changed; then measures this frame if the pass wrote it. A present it cannot measure
// -- the pass returned early, did not write, the slot is still in flight, the list cannot be tracked -- still took a
// sequence number in NotePulsePresent, so the pair across it is broken rather than bridged.
void UpdatePulse(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, bool enabled, ID3D12Resource* target,
                 ID3D12Resource* input, bool wrote, uint32_t width, uint32_t height, uint32_t format,
                 uint32_t cadenceRequested, bool cadenceSmoothing, float residualLimit, float minRatio,
                 uint32_t cadenceInEffect, bool carried, uint64_t present, int64_t nowMs)
{
    if (!enabled || !DlssNr_Pulse_Dx12::Available())
    {
        StopPulseLocked();
        return;
    }

    if (g_pulse.pass != nullptr && !g_pulse.pass->BuiltFor(device))
        StopPulseLocked();

    if (g_pulse.pass == nullptr)
    {
        // A failed pass is kept, so it is not rebuilt, and logged again, every frame.
        g_pulse.pass = new DlssNr_Pulse_Dx12("DLSS-NR pulse", device);
        g_pulse.active = true;
        if (!g_pulse.pass->IsInit())
            LOG_ERROR("DLSS-NR pulse: the measuring pass could not be built; the metric is off for this device");
        PublishPulse(g_pulse.pass->IsInit(), !g_pulse.pass->IsInit());
    }

    if (!g_pulse.pass->IsInit())
        return;

    // The readbacks whose recordings completed, in the order they were recorded.
    uint32_t done[DLSSNR_PULSE_SLOTS];
    uint32_t doneCount = 0;
    for (uint32_t i = 0; i < DLSSNR_PULSE_SLOTS; ++i)
    {
        if (g_pulse.slots[i].pending && DlssNr::Submission::Completed(g_pulse.slots[i].usage))
            done[doneCount++] = i;
    }
    std::sort(done, done + doneCount,
              [](uint32_t a, uint32_t b) { return g_pulse.slots[a].seq < g_pulse.slots[b].seq; });

    for (uint32_t k = 0; k < doneCount; ++k)
    {
        auto& slot = g_pulse.slots[done[k]];
        slot.pending = false;
        if (slot.stale)
            continue;

        if (g_pulse.tracker.Add(slot.seq, slot.ms, slot.kind, g_pulse.pass->Light(done[k]), slot.cadence))
        {
            PublishPulse(true, false);
            if (g_pulse.logGate.Due(nowMs))
                LOG_INFO("{}", g_pulse.tracker.Latest().LogLine());
        }
    }

    const PulseSession::Shape shape {
        width, height, format, cadenceRequested, cadenceRequested > 1 && cadenceSmoothing, residualLimit, minRatio
    };
    if (!g_pulse.shape || !(*g_pulse.shape == shape))
    {
        g_pulse.shape = shape;
        g_pulse.tracker.Reset();
        g_pulse.logGate.Reset();
        for (auto& slot : g_pulse.slots)
            slot.stale = slot.pending;
        PublishPulse(true, false);
    }

    // One measurement per present: a title that runs the pass twice in one shows two views, and a step between them
    // is not a step in time. Where presents are not counted (the counter stays 0) every call is measured.
    if (present != 0 && present == g_pulse.lastPresent)
        return;
    g_pulse.lastPresent = present;
    const uint64_t seq = g_pulse.entered;

    auto& slot = g_pulse.slots[g_pulse.next];
    if (wrote && target != nullptr && input != nullptr && !slot.pending)
    {
        if (!DlssNr::Submission::Track(cmdList, slot.usage))
        {
            if (!g_pulse.saidFailure)
                LOG_INFO("DLSS-NR pulse: this command list cannot be tracked ({}); frames on it are not measured",
                         DlssNr::Submission::LastRefusal());
            g_pulse.saidFailure = true;
        }
        else
        {
            // The target rests in UNORDERED_ACCESS between the pass's writes; the untouched copy is readable already.
            Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const bool recorded = g_pulse.pass->Record(cmdList, target, input, width, height, g_pulse.next);
            Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            if (recorded)
            {
                slot.pending = true;
                slot.stale = false;
                slot.seq = seq;
                slot.ms = nowMs;
                slot.kind = DlssNr::Pulse::KindOf(cadenceInEffect, carried);
                slot.cadence = cadenceRequested;
                g_pulse.next = (g_pulse.next + 1) % DLSSNR_PULSE_SLOTS;
            }
        }
    }
}

// Records the copy of this frame's grid into whichever readback buffer is furthest from being read.
// Same shape as the meter's copy, against the calibration surface and its own ring.
void CopyCalibrationToReadback(ID3D12GraphicsCommandList* cmdList)
{
    const unsigned int slot = (unsigned int) (g_nr.calibFrames % 4);

    if (g_nr.calibReadback[slot] == nullptr || g_nr.calib == nullptr)
        return;

    D3D12_TEXTURE_COPY_LOCATION srcLoc {};
    srcLoc.pResource = g_nr.calib;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.calibReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.calib, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    Barrier(cmdList, g_nr.calib, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.calibFrames++;
}

void CopyMeterToReadback(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, bool exposureBound)
{
    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);

    if (g_nr.meterReadback[slot] == nullptr)
        return;

    // Travels with the grid: read back three frames from now, alongside the tiles it describes.
    g_nr.meterExposureValid[slot] = exposureBound;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = g_nr.meter;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.meterReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.meterFrames++;
}

// Takes the game's exposure out of tile 0 of the grid recorded three frames ago.
//
// Only tile 0 is written now. The frame-statistics meter this served was removed: a divisor measured
// off a frame this pass writes is a feedback loop rather than a measurement. What is left is a
// courier -- the game's exposure is a 1x1 texture in a resource state this pass did not set and must
// not transition, so the shader reads it as an SRV and it rides home on a readback that exists.
// Reads the calibration grid written four frames ago and turns it into one number.
//
// A high percentile of tile peaks, not the maximum: the maximum is a sun or a specular hit and would
// normalise the whole picture into the dark. The 90th percentile is high enough to sit at the top of
// the real range and common enough that no single highlight decides it.
void ConsumeCalibrationReadback()
{
    if (g_nr.calibFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.calibFrames % 4);
    ID3D12Resource* buffer = g_nr.calibReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, kMeterBytes };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    std::vector<float> tiles;
    tiles.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);

    for (unsigned int i = 0; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
    {
        if (std::isfinite(src[i]) && src[i] > 1e-6f)
            tiles.push_back(src[i]);
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);

    if (tiles.size() < 16)
        return;

    const size_t nth = (size_t) ((float) (tiles.size() - 1) * 0.90f);
    std::nth_element(tiles.begin(), tiles.begin() + nth, tiles.end());

    // How much of the frame carries light, measured against its own brightest tile rather than an
    // absolute threshold -- the units here are the game's and there is no absolute scale.
    //
    // This is what separates "the buffer is scaled by 240" from "I am standing in a dark cave". A
    // percentile of tile peaks is a statement about scene content; it only describes the buffer when
    // enough of the picture is lit for the top of the range to actually appear in it.
    float brightest = 0.0f;

    for (float v : tiles)
        brightest = std::max(brightest, v);

    unsigned int lit = 0;

    for (float v : tiles)
    {
        if (v > brightest * 0.10f)
            ++lit;
    }

    const float litFraction = tiles.empty() ? 0.0f : (float) lit / (float) tiles.size();

    // A torn readback survives isfinite and would clamp to exactly the ceiling, which since the
    // ceiling became 2000 is a value the slider can hold -- so a garbage frame could be offered as a
    // real answer. Reject rather than clamp.
    if (!(tiles[nth] > 0.0f) || tiles[nth] >= 1999.0f)
        return;

    const float suggestion = std::clamp(tiles[nth], 0.25f, 1990.0f);

    g_nr.calibUsable = !g_nr.calibPassthrough && litFraction > 0.20f;
    g_nr.calibWhy = g_nr.calibPassthrough
                        ? "this game hands over a frame it already tone mapped, so there is nothing to normalise"
                    : litFraction <= 0.20f ? "too little of this scene is lit to say where the top of the range is"
                                           : "";

    g_nr.calibHistory[g_nr.calibCount % NrState::kCalibHistory] = suggestion;
    g_nr.calibCount++;
    g_nr.calibSuggestion = suggestion;

    // Confidence is the spread of recent answers, not their absolute size. A number that has held
    // still for a second is one worth taking; one that is swinging means the scene is changing under
    // the measurement, and no single value would serve anyway.
    const unsigned int have = std::min<unsigned int>(g_nr.calibCount, NrState::kCalibHistory);

    if (have >= 8)
    {
        float lo = g_nr.calibHistory[0];
        float hi = g_nr.calibHistory[0];

        for (unsigned int i = 0; i < have; ++i)
        {
            lo = std::min(lo, g_nr.calibHistory[i]);
            hi = std::max(hi, g_nr.calibHistory[i]);
        }

        // A spread of 1.0x is perfect agreement and 2x or worse is none.
        const float spread = hi / lo;
        g_nr.calibSteadiness = std::clamp(1.0f - (spread - 1.0f), 0.0f, 1.0f);
    }
}

void ConsumeMeterReadback()
{
    if (g_nr.meterFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);
    ID3D12Resource* buffer = g_nr.meterReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, sizeof(float) };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    // Only believed when the frame that wrote this grid actually had an exposure texture bound. With
    // nothing bound DispatchPass substitutes the source picture, and tile 0 is then a scene pixel
    // rather than an exposure -- believing it made the white point follow the top-left corner of the
    // screen, which in Cyberpunk moved by up to 272x between frames and flashed the whole picture.
    //
    // When it is not believed gameExposure keeps its last good value, or stays 0 and lets
    // ResolveWhitePoint fall back to the slider, which is what a game supplying none should get.
    if (g_nr.meterExposureValid[slot] && DlssNr::Exposure::ValidSample(src[0]))
    {
        g_nr.gameExposure = src[0];

        // The game's own implied white, without the user's trim: sweeping the trim across its 0.01..4 range
        // would itself read as a nine-stop swing.
        const float implied = DlssNr::Exposure::PreExposure(g_nr.gamePreExposure) / src[0];
        if (g_nr.exposureLatch.Observe(implied))
        {
            LOG_WARN("DLSS-NR: the game's exposure is not usable (implied white point as low as {:.4f} and as high "
                     "as {:.1f}; {} readings below 0.02, {} above 512); using paper white for the rest of the session",
                     g_nr.exposureLatch.Lowest(), g_nr.exposureLatch.Highest(), g_nr.exposureLatch.LowCount(),
                     g_nr.exposureLatch.HighCount());
        }
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);
}

// Forget everything the meter knows, so nothing read before this moment can be believed after it.
//
// The exposure is written only inside the block that dispatches the meter, and that block does not
// run while the option is off. Nothing used to clear any of this when it stopped, so the reading
// simply froze: switching the option back on returned the value from whenever it was switched off,
// and ResolveWhitePoint took it as current because a held value is exactly what it expects to see.
// GTA V's exposure spans 0.127 to 0.511 in one session, so re-enabling in different light handed the
// encode a white point up to 4x wrong -- which trips the soft knee, scales the model's answer away
// and leaves its hue behind. That is the colour cast, and it looked random because it depends on the
// light at the moment of the PREVIOUS switch-off, which nothing on screen shows.
//
// The readback ring made it worse. `meterFrames` also only advances inside that block, so the four
// slots kept their contents and their valid flags across the gap, and the first frames after
// re-enabling consumed buffers written before it as though they had just arrived.
//
// Zero is not a fallback value here, it is the absence of one: ResolveWhitePoint's `> 1e-6f` guard
// fails and the manual slider is used, which is what a game supplying no exposure already gets.
void InvalidateExposureMeter()
{
    g_nr.gameExposure = 0.0f;

    for (bool& valid : g_nr.meterExposureValid)
        valid = false;

    // Re-arms the `< 4` guard in ConsumeMeterReadback, so nothing is read back until four frames
    // have genuinely been queued since this point.
    g_nr.meterFrames = 0;
    g_nr.exposureLatch.Reset();
}

// Keep source selection testable without changing the established exposure law.
float ResolveWhitePoint(int whitePointSource, float whitePointScale, float whitePointTrim, bool isHdrBuffer,
                        bool scanInverted, float scanTrim)
{
    const float anchored =
        isHdrBuffer && whitePointSource == 2
            ? DlssNr::ExposureScan::AnchoredWhitePoint(DlssNr::ExposureScan::BestValue(), scanInverted, scanTrim)
            : 0.0f;
    return DlssNr::Exposure::WhitePoint(whitePointSource, isHdrBuffer, whitePointScale, whitePointTrim,
                                        g_nr.gameExposure, g_nr.gamePreExposure, anchored,
                                        g_nr.exposureLatch.Latched());
}

// Every way out of the pass before it does anything is silent on purpose -- an evaluate that carries
// no depth is normal and would otherwise print every frame forever. That silence is fine until the
// pass does nothing at all and the log has no opinion about why.
//
// So each distinct reason is reported once. Once, not once per frame.
void ReportSkipOnce(const char* reason)
{
    static std::vector<const char*> seen;

    if (reason == nullptr)
        reason = "(null reason)";
    DlssNr::Replay::NoteSkip(reason);

    for (const char* logged : seen)
    {
        if (std::strcmp(logged, reason) == 0)
            return;
    }

    seen.push_back(reason);
    LOG_INFO("DLSS-NR did not run: {}", reason);
}
} // namespace DlssNr::Dx12Internal
