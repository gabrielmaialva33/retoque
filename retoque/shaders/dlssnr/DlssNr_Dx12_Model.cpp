#include "pch.h"

#include "DlssNr_Dx12_Internal.h"
#include <dlssnr/DlssNr_DirectRuntime.h>
#include <dlssnr/DlssNr_Consumers.h>
#include <dlssnr/DlssNr_ModelLog.h>
#include <dlssnr/DlssNr_Session.h>
#include <proxies/NVNGX_Proxy.h>
#include <State.h>
#include <Util.h>
#include <dxgi1_4.h>
#include <cstdio>

namespace DlssNr::Dx12Internal
{
// NGX result codes, by name.
//
// A user's log recently read "init 0x-452FFFFF", which is an int formatted as hex and is
// undiagnosable by anyone. It was 0xBAD00001, FeatureNotSupported -- a complete answer, printed as
// noise. Names cost nothing and turn a bug report into a diagnosis.
const char* NgxResultName(unsigned int r)
{
    switch (r)
    {
    case 0x1:
        return "Success";
    case 0xBAD0FA17:
        return "forwarder: the model faulted, call refused";
    case 0xBAD00001:
        return "FAIL_FeatureNotSupported";
    case 0xBAD00002:
        return "FAIL_PlatformError";
    case 0xBAD00003:
        return "FAIL_FeatureAlreadyExists";
    case 0xBAD00004:
        return "FAIL_FeatureNotFound";
    case 0xBAD00005:
        return "FAIL_InvalidParameter";
    case 0xBAD00006:
        return "FAIL_ScratchBufferTooSmall";
    case 0xBAD00007:
        return "FAIL_NotInitialized";
    case 0xBAD00008:
        return "FAIL_UnsupportedInputFormat";
    case 0xBAD00009:
        return "FAIL_RWFlagMissing";
    case 0xBAD0000A:
        return "FAIL_MissingInput";
    case 0xBAD0000B:
        return "FAIL_UnableToInitializeFeature";
    case 0xBAD0000C:
        return "FAIL_OutOfDate";
    case 0xBAD0000D:
        return "FAIL_OutOfGPUMemory";
    case 0xBAD0000E:
        return "FAIL_UnsupportedFormat";
    case 0xBAD0000F:
        return "FAIL_UnableToWriteToAppDataPath";
    case 0xBAD00010:
        return "FAIL_UnsupportedParameter";
    case 0xBAD00011:
        return "FAIL_Denied";
    case 0xBAD00012:
        return "FAIL_NotImplemented";
    default:
        return "unknown";
    }
}

// Does the driver's own nvngx.dll dispatch Neural Rendering?
//
// The trick is that correct parameters are not needed to find out, because the KIND of failure is
// the answer. A dispatcher that has never heard of feature 18 rejects it before looking at anything:
//
//   FeatureNotFound / FeatureNotSupported / NotImplemented -- the driver does not route it, and the
//       forwarder is necessary rather than merely tolerated.
//   MissingInput / InvalidParameter / UnsupportedParameter -- the driver DOES route it. It reached
//       the feature, which then complained about the arguments. That is the win: it means the whole
//       forwarder, and the per-game copy of the model, can go.
//   Success -- better still, though not expected from an empty parameter block.
//
// Once per session, and only when asked for.
void ProbeProxyDispatch(ID3D12GraphicsCommandList* cmdList)
{
    static bool done = false;

    if (done)
        return;

    done = true;

    if (!NVNGXProxy::IsDx12Inited())
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx is not initialised here, nothing to ask");
        return;
    }

    const auto allocate = NVNGXProxy::D3D12_AllocateParameters();
    const auto destroy = NVNGXProxy::D3D12_DestroyParameters();
    const auto create = NVNGXProxy::D3D12_CreateFeature();
    const auto release = NVNGXProxy::D3D12_ReleaseFeature();

    if (allocate == nullptr || create == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx does not export what the probe needs");
        return;
    }

    NVSDK_NGX_Parameter* params = nullptr;

    if (allocate(&params) != NVSDK_NGX_Result_Success || params == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: could not allocate a parameter block");
        return;
    }

    // Feature 18, and a feature that certainly does not exist, asked the same way.
    //
    // A single result cannot answer this. "UnableToInitializeFeature" for 18 looks like the
    // dispatcher having found the feature and failed to start it on an empty parameter block -- but
    // it might equally be what this dispatcher says about anything it cannot set up. The control
    // settles it: if a nonsense id comes back differently, the difference is knowledge of feature
    // 18. If both come back the same, the first result meant nothing.
    NVSDK_NGX_Handle* handle = nullptr;
    const auto result = (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 18, params, &handle);

    if (handle != nullptr && release != nullptr)
        release(handle);

    NVSDK_NGX_Handle* controlHandle = nullptr;
    const auto control = (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 200, params, &controlHandle);

    if (controlHandle != nullptr && release != nullptr)
        release(controlHandle);

    LOG_INFO("DLSS-NR proxy probe: feature 18 -> 0x{:X} ({}), control feature 200 -> 0x{:X} ({})", result,
             NgxResultName(result), control, NgxResultName(control));

    const bool rejectedOutright = result == 0xBAD00004 || result == 0xBAD00001 || result == 0xBAD00012;

    if (result == control)
        LOG_INFO("DLSS-NR proxy probe: both answers identical, so this says nothing about feature 18 "
                 "-- the driver treats it exactly as it treats a feature that does not exist");
    else if (rejectedOutright)
        LOG_INFO("DLSS-NR proxy probe: feature 18 is rejected outright -- the driver does not route "
                 "it and the forwarder is required");
    else
        LOG_INFO("DLSS-NR proxy probe: feature 18 answers differently from a nonexistent one, so the "
                 "driver knows it -- the forwarder and the per-game model copy could both go");

    if (destroy != nullptr)
        destroy(params);
}

// [DlssNr] ModelLoader=direct: the same entry points, made from this module. See DlssNr_DirectRuntime.h.
bool EnsureDirectRuntime()
{
    namespace Direct = DlssNr::DirectRuntime;

    g_nr.queryRatio = &Direct::QueryScalingRatio;
    g_nr.lastRatioStage = Direct::LastRatioStage();
    g_nr.create = &Direct::Create;
    g_nr.evaluate = &Direct::Evaluate;
    g_nr.release = &Direct::Release;
    g_nr.setExtras = &Direct::SetExtras;
    g_nr.setFloatSlot = &Direct::SetFloatSlot;
    g_nr.probeFloat = &Direct::ProbeFloat;
    g_nr.lastInit = Direct::LastInit();
    g_nr.lastCreate = Direct::LastCreate();
    g_nr.faultState = &Direct::FaultState;
    g_nr.directRuntime = true;

    LOG_INFO("DLSS-NR: model loader is direct -- OptiScaler calls nvngx_dlssnr.dll itself on the D3D12 route, no "
             "nvngx.dll_dlssnr.dll needed there (native Vulkan and the D3D11 probe still use it)");

    DlssNr::Consumers::WarnIfCompeting();
    return true;
}

// Loads the forwarder that owns the calls into the snippet.
bool EnsureForwarder()
{
    if (g_nr.directRuntime)
        return g_nr.create != nullptr;

    if (g_nr.forwarder != nullptr)
        return g_nr.create != nullptr;

    if (Config::Instance()->DlssNrModelLoader.value_or_default() == NrModelLoader::Direct)
        return EnsureDirectRuntime();

    if (g_dllDir.empty())
        g_dllDir = Util::DllPath().remove_filename();

    // Beside OptiScaler first, then beside the executable: someone dropping this into a game folder may
    // reasonably put it in either place.
    auto found = Util::FindFilePath(g_dllDir, "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
        found = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll not found beside OptiScaler ({}) or the game executable", g_dllDir.string());
        g_nr.reason = "nvngx.dll_dlssnr.dll is missing";
        return false;
    }

    // FindFilePath hands back the file itself, not the directory holding it.
    const auto path = found.value();
    g_nr.forwarder = LoadLibraryW(path.wstring().c_str());

    if (g_nr.forwarder == nullptr)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll found at {} but would not load, error {}", path.string(), GetLastError());
        g_nr.reason = "nvngx.dll_dlssnr.dll would not load";
        return false;
    }

    g_nr.queryRatio = (int (*)(const wchar_t*, void*, unsigned int, float*)) GetProcAddress(
        g_nr.forwarder, "dlssnr_query_scaling_ratio");
    g_nr.lastRatioStage = (const int*) GetProcAddress(g_nr.forwarder, "dlssnr_last_ratio_stage");

    g_nr.create = (PFN_NrCreate) GetProcAddress(g_nr.forwarder, "dlssnr_call_create");
    g_nr.evaluate = (PFN_NrEvaluate) GetProcAddress(g_nr.forwarder, "dlssnr_call_evaluate");
    g_nr.release = (PFN_NrRelease) GetProcAddress(g_nr.forwarder, "dlssnr_call_release");
    // Optional: an older forwarder simply lacks it, and the model runs as before.
    g_nr.setExtras = (PFN_NrSetExtras) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_extras");
    g_nr.setFloatSlot = (PFN_NrSetFloatSlot) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_float_slot");
    g_nr.probeFloat = (PFN_NrProbeFloat) GetProcAddress(g_nr.forwarder, "dlssnr_call_probe_float");
    g_nr.lastInit = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_init");
    g_nr.lastCreate = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_create");
    g_nr.faultState = (int (*)(unsigned long*, void**)) GetProcAddress(g_nr.forwarder, "dlssnr_fault_state");

    if (g_nr.create == nullptr || g_nr.evaluate == nullptr)
    {
        g_nr.reason = "the forwarder is missing its exports";
        return false;
    }

    // Refused rather than called, because calling it would work. See kDlssNrForwarderAbi: the
    // argument lists have changed while the export names stayed the same, so a forwarder left over
    // from an earlier build answers GetProcAddress and then reads every argument past the fourth from
    // the wrong stack slot. That does not fault -- it quietly ruins the picture -- and the file is
    // copied per game folder, so one directory updated by hand is all it takes.
    const auto* forwarderAbi = (const int*) GetProcAddress(g_nr.forwarder, "dlssnr_abi_version");

    if (forwarderAbi == nullptr || *forwarderAbi != kDlssNrForwarderAbi)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll at {} is version {} and this build needs {}; copy the forwarder "
                  "from the same build as OptiScaler",
                  path.string(), forwarderAbi == nullptr ? 1 : *forwarderAbi, kDlssNrForwarderAbi);
        g_nr.reason = "nvngx.dll_dlssnr.dll is from a different build";
        return false;
    }

    // Said rather than asked, so the check works in the other direction too: an OptiScaler.dll rolled
    // back from dxgi.dll.prev never calls this, and the forwarder refuses a host it has not heard from.
    if (const auto setHostAbi = (void(__cdecl*)(int)) GetProcAddress(g_nr.forwarder, "dlssnr_set_host_abi"))
        setHostAbi(kDlssNrForwarderAbi);

    LOG_INFO("DLSS-NR forwarder loaded from {}", path.string());

    // The neural pass is live from here, which is the only moment worth asking whether something
    // else in this process is also running one. See DlssNr_Consumers.h for why two is a wrong image.
    DlssNr::Consumers::WarnIfCompeting();

    return true;
}

// The model needs the driver core's own capability block: it carries the snippet and preset callbacks a
// feature expects at create time, which a freshly allocated block does not have.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params);
// Whether the forwarder has caught a fault inside the model, reported once with its exception code
// and faulting module. Checked independently of g_nr.failed, because a faulted call comes back as an
// ordinary failed result (0xBAD0FA17) and the paths that handle one set failed first: gating this on
// !failed was how the one line that says what happened never got written.
bool NoteModelFault()
{
    static bool reported = false;
    unsigned long code = 0;
    void* address = nullptr;

    if (g_nr.faultState == nullptr || g_nr.faultState(&code, &address) == 0)
        return false;

    g_nr.failed = true;
    g_nr.reason = "the model faulted inside its own code; off for this session";
    DlssNr::SessionGuardOff(); // NR is not running any more, so what follows is not an NR crash

    if (!reported)
    {
        reported = true;
        LOG_ERROR("DLSS-NR: {} ({})", g_nr.reason, DlssNr::ModelLog::DescribeFault(code, address));
    }

    return true;
}

void ReportScalingRatios();

bool EnsureCapabilityParams(ID3D12Device* device)
{
    if (g_nr.capabilityParams != nullptr)
        return true;

    if (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device))
    {
        g_nr.reason = "the NGX core would not initialise";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters() == nullptr)
    {
        g_nr.reason = "the NGX core has no capability parameters";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters()(&g_nr.capabilityParams) != NVSDK_NGX_Result_Success ||
        g_nr.capabilityParams == nullptr)
    {
        g_nr.capabilityParams = nullptr;
        g_nr.reason = "the NGX core refused its capability parameters";
        return false;
    }

    // Before anything is written to it, work out where this block keeps floats.
    DiscoverFloatSlot(g_nr.capabilityParams);

    // Ask the model what scaling ratio it wants, once, for every quality level it might accept.
    //
    // Read-only and answered before any feature exists. The point is to find out whether NVIDIA's own
    // performance mode for this model is reachable: the snippet has ComputeScalingRatioCommon and the
    // kernel table has _ds, _upsample and _upsample_tilesync variants of every fused Swin block, which
    // together suggest the model can run its interior below display resolution natively -- rather than
    // being handed a picture we shrank ourselves, which costs an extra resample of the edit on the way
    // back and quantises the Swin grid to a lattice we chose rather than the one it was trained on.
    ReportScalingRatios();
    return true;
}

// What the model says it wants to run at, per quality level. Logged once, used for nothing yet.
//
// Answered by the snippet's own callback rather than chosen by us. If it answers, NVIDIA ships a
// performance mode for Neural Rendering and the resolution slider is a worse hand-rolled version of
// it. If it does not, the slider is all there is and that is worth knowing too.
void ReportScalingRatios()
{
    if (g_nr.queryRatio == nullptr || g_nr.capabilityParams == nullptr)
        return;

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        return;

    static const char* kNames[] = { "MaxPerf", "Balanced", "MaxQuality", "UltraPerformance", "UltraQuality", "DLAA" };

    char line[512] = {};
    size_t used = 0;
    bool any = false;

    for (unsigned int q = 0; q < 6; ++q)
    {
        float ratio = -1.0f;
        const int rc = g_nr.queryRatio(snippet->wstring().c_str(), g_nr.capabilityParams, q, &ratio);
        int written = 0;

        if (rc == 1)
        {
            any = true;
            written = snprintf(line + used, sizeof(line) - used, "%s=%.4f ", kNames[q], ratio);
        }
        else if (rc == -1)
        {
            written = snprintf(line + used, sizeof(line) - used, "%s=refused ", kNames[q]);
        }

        if (written > 0)
            used += (size_t) written;
    }

    if (any)
        LOG_INFO("DLSS-NR the model's own scaling ratios: {}", line);
    else
        LOG_INFO("DLSS-NR scaling ratio callback not published by this snippet (stage {})",
                 g_nr.lastRatioStage != nullptr ? *g_nr.lastRatioStage : -1);
}

// Works out which vtable slot this parameter block keeps floats in, by writing a known value through
// each candidate and asking for it back through the header's typed getter. Only a slot that returns the
// value it was given is accepted.
//
// Slot 1 is where the public header declares the float overload, so it is tried first and wins wherever
// that assumption holds. It does not hold for the driver's own block: every float written there reads
// back as FAIL_UnsupportedParameter while every uint lands, which is why intensity, local structure,
// local tone and skin structure never did anything.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params)
{
    if (g_nr.floatSlotKnown || params == nullptr || g_nr.probeFloat == nullptr || g_nr.setFloatSlot == nullptr)
        return;

    g_nr.floatSlotKnown = true;

    static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
    static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    const float expected = 0.375f; // exact in binary, so the round trip is exact or it is wrong

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_nr.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_nr.setFloatSlot(slot);
            LOG_INFO("DLSS-NR float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR could not find the float setter: intensity, local structure, local tone and skin "
              "structure will have no effect. The uint parameters still apply.");
}

// The extras the official integration sets: global tone (read at create) and the interface inputs.
// Written before every create and evaluate, nulls included, so nothing stale ever sits in the block.
void SetExtras(const Config& cfg, ID3D12Resource* ui, ID3D12Resource* backbuffer, unsigned int uiWidth,
               unsigned int uiHeight, unsigned int bbWidth, unsigned int bbHeight)
{
    if (g_nr.setExtras == nullptr || g_nr.capabilityParams == nullptr)
        return;

    // Global tone is written at the model's own default: the control that exposed it changed nothing
    // that could be seen, and the block persists, so a value still has to be put there.
    g_nr.setExtras(g_nr.capabilityParams, 1.0f, ui, ui, backbuffer, uiWidth, uiHeight, bbWidth, bbHeight);
}

bool TuningMatchesFeature(const Config& cfg)
{
    return g_nr.builtPreset == cfg.DlssNrPreset.value_or_default() &&
           g_nr.builtIntensity == cfg.DlssNrIntensity.value_or_default() &&
           g_nr.builtStyle == cfg.DlssNrStyle.value_or_default() &&
           g_nr.builtLocalStructure == cfg.DlssNrLocalStructure.value_or_default() &&
           g_nr.builtLocalTone == cfg.DlssNrLocalTone.value_or_default() &&
           g_nr.builtSkinStructure == cfg.DlssNrSkinStructure.value_or_default() &&
           g_nr.builtAutoMask == cfg.DlssNrAutoMask.value_or_default();
}

void RecordBuiltTuning(const Config& cfg)
{
    g_nr.builtPreset = cfg.DlssNrPreset.value_or_default();
    g_nr.builtIntensity = cfg.DlssNrIntensity.value_or_default();
    g_nr.builtStyle = cfg.DlssNrStyle.value_or_default();
    g_nr.builtLocalStructure = cfg.DlssNrLocalStructure.value_or_default();
    g_nr.builtLocalTone = cfg.DlssNrLocalTone.value_or_default();
    g_nr.builtSkinStructure = cfg.DlssNrSkinStructure.value_or_default();
    g_nr.builtAutoMask = cfg.DlssNrAutoMask.value_or_default();
}

} // namespace DlssNr::Dx12Internal

namespace DlssNr
{
using namespace Dx12Internal;

void ProbeD3D11(void* d3d11Device)
{
    static bool done = false;

    if (done || d3d11Device == nullptr)
        return;

    // Every other entry point in this file takes the lock before touching g_nr; this one was reaching
    // EnsureForwarder without it.
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    // Opt in only. See the note on DlssNrProbeD3D11: this is the one call in the pass that reaches
    // into a subsystem on the game's own device rather than reading something we already hold.
    if (!Config::Instance()->DlssNrProbeD3D11.value_or_default())
        return;

    done = true;

    if (!EnsureForwarder())
        return;

    // The probe's entry points exist only in the forwarder. ModelLoader=direct does not load it.
    if (g_nr.forwarder == nullptr)
    {
        LOG_INFO("DLSS-NR D3D11: the probe needs the forwarder, and ModelLoader=direct does not load it");
        return;
    }

    auto probe = (int (*)(const wchar_t*)) GetProcAddress(g_nr.forwarder, "dlssnr_d3d11_probe");
    auto init = (int (*)(const wchar_t*, const wchar_t*, void*, int, int*, int*)) GetProcAddress(g_nr.forwarder,
                                                                                                 "dlssnr_d3d11_init");

    if (probe == nullptr || init == nullptr)
    {
        LOG_INFO("DLSS-NR D3D11: this forwarder has no D3D11 probe");
        return;
    }

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        return;

    // Four bits, one per entry point: init 1, create 2, evaluate 4, release 8.
    const int bits = probe(snippet->wstring().c_str());

    // And the question NGX has an API for. Asked first because it creates nothing: if the feature
    // declines D3D11 here, that is the feature's own answer rather than our reading of a failed init.
    auto requirements = (int (*)(const wchar_t*, void*, unsigned int*, unsigned int*, unsigned int*)) GetProcAddress(
        g_nr.forwarder, "dlssnr_d3d11_requirements");

    if (requirements != nullptr)
    {
        // The adapter the game is actually running on. Without it the query answers
        // AdapterUnsupported, which looks like a verdict on the hardware and is really a verdict on
        // the question -- that is what the first attempt got, on a 5080.
        //
        // Read it from the game's own device. Adapter 0 is whichever card drives the primary
        // display, and on a two-GPU machine (a second card for Lossless Scaling, the monitor on
        // it) that is not the card the game renders on: the question was then asked about a GPU
        // nothing here would ever use, and its AdapterUnsupported answer disabled the route.
        IDXGIAdapter* adapter = nullptr;
        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIFactory1* factory = nullptr;

        if (SUCCEEDED(static_cast<IUnknown*>(d3d11Device)->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
            dxgiDevice != nullptr)
        {
            if (FAILED(dxgiDevice->GetAdapter(&adapter)))
                adapter = nullptr;

            dxgiDevice->Release();
        }

        if (adapter == nullptr)
        {
            LOG_WARN("DLSS-NR D3D11: the game's device did not give up its adapter, asking about adapter 0");

            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr)
                factory->EnumAdapters(0, &adapter);
        }

        DXGI_ADAPTER_DESC adapterDesc {};
        if (adapter != nullptr && FAILED(adapter->GetDesc(&adapterDesc)))
            adapterDesc = {};

        unsigned int supported = 0xFFFFFFFFu;
        unsigned int minArch = 0;
        unsigned int minOs = 0;
        const int rc = requirements(snippet->wstring().c_str(), adapter, &supported, &minArch, &minOs);

        const char* meaning = supported == 0     ? "SUPPORTED"
                              : (supported & 16) ? "NotImplemented -- the feature has no D3D11 path"
                              : (supported & 4)  ? "AdapterUnsupported"
                              : (supported & 2)  ? "DriverVersionUnsupported"
                              : (supported & 8)  ? "OSVersionBelowMinimum"
                              : (supported & 1)  ? "CheckNotPresent"
                                                 : "unknown";

        LOG_WARN("DLSS-NR D3D11: GetFeatureRequirements {} ({}), FeatureSupported 0x{:X} -- {}. "
                 "minimum architecture 0x{:X}, minimum OS 0x{:X}, asked about adapter vendor {:04X} device {:04X}",
                 rc, NgxResultName((unsigned int) rc), supported, meaning, minArch, minOs, adapterDesc.VendorId,
                 adapterDesc.DeviceId);

        if (adapter != nullptr)
            adapter->Release();

        if (factory != nullptr)
            factory->Release();
    }

    LOG_INFO("DLSS-NR D3D11: entry points resolved {}/15 (init {}, create {}, evaluate {}, release {})", bits,
             (bits & 1) ? "yes" : "no", (bits & 2) ? "yes" : "no", (bits & 4) ? "yes" : "no",
             (bits & 8) ? "yes" : "no");

    if (bits != 15)
    {
        LOG_INFO("DLSS-NR D3D11: incomplete surface, the bridge stays the only route");
        return;
    }

    // Four ways of asking, since the feature has already said it supports this platform.
    int attempt = 0;
    int results[4] = { -9, -9, -9, -9 };

    const int result = init(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                            d3d11Device, 0x0000015, &attempt, results);

    static const char* kNames[4] = { "Init_Ext on our own copy", "Init on our own copy",
                                     "Init_Ext on the shared module", "Init on the shared module" };

    for (int i = 0; i < 4; ++i)
    {
        LOG_INFO("DLSS-NR D3D11:   {} -> {} ({})", kNames[i], results[i],
                 results[i] == -2   ? "module not loaded"
                 : results[i] == -3 ? "export missing"
                 : results[i] == -9 ? "not reached"
                                    : NgxResultName((unsigned int) results[i]));
    }

    if (result == 1)
        LOG_WARN("DLSS-NR D3D11: initialised, via {}. The feature already said this platform is "
                 "supported; now the call works too. Next is a feature create on a device context.",
                 attempt > 0 ? kNames[attempt - 1] : "?");
    else
        // Deliberately not "so the bridge is required". GetFeatureRequirements answers 0x0 SUPPORTED
        // with a minimum architecture this card meets, so the platform is not the obstacle and saying
        // otherwise here would be printing a conclusion the evidence does not carry.
        LOG_WARN("DLSS-NR D3D11: every init variant refused, last {} ({}) -- though the feature itself "
                 "reports this platform as supported, so the obstacle is in how it is being called",
                 result, NgxResultName((unsigned int) result));
}

} // namespace DlssNr
