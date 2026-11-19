#pragma once
// Fixture-only runtime data. No DLL discovery, GPU calls, settings persistence or clipboard writes.
#include <misc/Localization.h>
#include <dlssnr/DlssNr_AdvisorRuntime.h>
#include <dlssnr/DlssNr_AdvisorPolicy.h>
#include <dlssnr/DlssNr_PassSettings.h>
#include <shaders/dlssnr/DlssNr_Common.h>
#include <shaders/dlssnr/DlssNr_Periphery.h>
#include <dlssnr/DlssNr_Cadence.h>
#include <dlssnr/DlssNr_DiagnosticReport.h>
#include <dlssnr/DlssNr_DriverGate.h>
#include <dlssnr/DlssNr_Dx11Route.h>
#include <dlssnr/DlssNr_Enlarge.h>
#include <dlssnr/DlssNr_Funnel.h>
#include <dlssnr/DlssNr_Looks.h>
#include <dlssnr/DlssNr_ModelLog.h>
#include <dlssnr/DlssNr_ModelLogParse.h>
#include <dlssnr/DlssNr_ModelStats.h>
#include <dlssnr/DlssNr_NgxInfo.h>
#include <dlssnr/DlssNr_PairCapture.h>
#include <dlssnr/DlssNr_DetailStats.h>
#include <dlssnr/DlssNr_ReplayRecorder.h>
#include <dlssnr/DlssNr_Session.h>
struct D3D12_RESOURCE_DESC;
#include <dlssnr/DlssNr_ExposureScan.h>
#include "types.generated.h"

enum HasDefaultValue
{
    WithDefault,
    NoDefault,
    SoftDefault
};
template <class T, HasDefaultValue = WithDefault> struct CustomOptional : std::optional<T>
{
    T fallback {};
    CustomOptional() = default;
    CustomOptional(T v) : fallback(v) {}
    T value_or_default() const { return this->value_or(fallback); }
    CustomOptional& operator=(std::optional<T> v)
    {
        std::optional<T>::operator=(v);
        return *this;
    }
    CustomOptional& operator=(T v)
    {
        this->emplace(v);
        return *this;
    }
};
struct Config
{
#include "config.generated.h"
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
    DlssNr::PassConfig::Overrides overrides;
    DlssNrResolvedPassSettings GetDlssNrMasterSettings() const
    {
        DlssNrResolvedPassSettings s;
        s.Preset = DlssNrPreset.value_or_default();
        s.Style = DlssNrStyle.value_or_default();
        s.Intensity = DlssNrIntensity.value_or_default();
        s.LocalStructure = DlssNrLocalStructure.value_or_default();
        s.LocalTone = DlssNrLocalTone.value_or_default();
        s.SkinStructure = DlssNrSkinStructure.value_or_default();
        s.AutoMask = DlssNrAutoMask.value_or_default();
        return s;
    }
    DlssNrPassSnapshot GetDlssNrPassSnapshot() const
    {
        return { DlssNrPasses.value_or_default(), DlssNrIndividualPassSettings.value_or_default() };
    }
    DlssNrPassSettings GetDlssNrPassOverrides(uint32_t i) const { return DlssNr::PassConfig::Get(overrides, i); }
    void SetDlssNrPassOverrides(uint32_t i, DlssNrPassSettings s) { DlssNr::PassConfig::Set(overrides, i, s); }
    void SetDlssNrPassCount(uint32_t n) { DlssNrPasses = n; }
    void SetDlssNrIndividualPassSettings(bool x) { DlssNrIndividualPassSettings = x; }
    template <class T> void SetDlssNrMasterSetting(CustomOptional<T> Config::* m, T v) { this->*m = v; }
    struct Timing
    {
        bool Enabled;
        unsigned Interval;
    };
    bool DlssNrDx11SceneStageAtStart() const { return false; }
    Timing GetDlssNrGpuTimingSettings() const
    {
        return { DlssNrGpuTiming.value_or_default(), DlssNrGpuTimingInterval.value_or_default() };
    }
    void SetDlssNrGpuTimingEnabled(bool x) { DlssNrGpuTiming = x; }
    void SetDlssNrGpuTimingInterval(unsigned x) { DlssNrGpuTimingInterval = x; }
};
enum class API
{
    DX11,
    DX12,
    Vulkan
};
struct Feature
{
    bool IsWithDx12() const { return true; }
    const char* Name() const { return "DLSS (fixture)"; }
};
struct State
{
    API api = API::DX12;
    bool isRunningOnLinux = false;
    Feature* currentFeature = nullptr;
    static State& Instance()
    {
        static State s;
        return s;
    }
};
namespace Localization
{
inline Dictionary dictionary;
inline std::unordered_map<std::string, std::string> labels;
inline const Dictionary& Active() { return dictionary; }
inline const char* Tr(const char* text) { return dictionary.Find(text); }
inline const char* Label(const char* text)
{
    return labels.try_emplace(text, dictionary.LabelValue(text)).first->second.c_str();
}
} // namespace Localization
namespace MenuCommon
{
inline bool _hdrTonemapApplied = false;
inline void MenuHdrCheck(ImGuiIO&) {}
void ApplyThemeStyle();
} // namespace MenuCommon
namespace BuildInfo
{
inline const char* ProductName() { return "Retoque menu preview (synthetic)"; }
} // namespace BuildInfo
namespace Util
{
inline std::filesystem::path DllPath() { return {}; }
} // namespace Util
namespace Lsfg
{
inline auto GetStatus() { return DlssNr::DiagnosticReport::Inputs {}.lsfg; }
} // namespace Lsfg
namespace DlssNr
{
inline bool running = true, nativeVk = false;
inline const char* failure = "";
inline PresentHdrStatus hdr;
inline bool IsRunning() { return running && !nativeVk; }
inline bool IsRunningVk() { return running && nativeVk; }
inline const char* FailureReason() { return failure; }
inline void RetryAfterFailure() {}
inline void RetryEnlargement() {}
inline unsigned long long FramesVk() { return 120; }
inline std::optional<double> LastGpuTimeVk() { return {}; }
inline std::optional<unsigned long long> ModelVramBytes() { return {}; }
inline std::optional<unsigned long long> ModelVramBytesVk() { return {}; }
inline bool ExposureLatchedVk() { return false; }
inline bool ExposureOfferedVk() { return false; }
inline bool LinearHdrVk() { return true; }
inline bool BufferIsLinearHdr() { return !hdr.converted; }
inline bool RunningAfterRayReconstruction() { return false; }
inline bool ObservedRayReconstructionRoute() { return false; }
inline float WorkingScale() { return Config::Instance()->DlssNrWorkingScale.value_or_default(); }
inline float RRWorkingScale() { return 1; }
inline const char* BeforeUpscaleStatus() { return ""; }
inline unsigned ActivePassCount() { return running ? Config::Instance()->DlssNrPasses.value_or_default() : 0; }
inline const char* MultipassStatus() { return ""; }
inline const char* CadenceStatus() { return ""; }
inline bool CadenceAvailable() { return true; }
inline std::atomic_bool g_presentSyncWaits { false };
inline const char* HookStatus() { return hdr.converted ? "Present: active (fixture)" : "Upscaled: active (fixture)"; }
inline std::string PresentGuidesLine() { return {}; }
inline std::string SettingsContractText() { return {}; }
inline PresentHdrStatus GetPresentHdrStatus() { return hdr; }
inline PeripheryState PeripheryStatus() { return {}; }
inline Enlarge::Status EnlargeStatus() { return {}; }
inline ExposureStatus GameExposureStatus() { return {}; }
inline CalibrationReading Calibration() { return {}; }
inline DetailMeasureStatus GetDetailMeasureStatus() { return {}; }
inline bool DetailMeasureAvailable() { return running; }
inline void StartMeasureDetail() {}
inline void CancelMeasureDetail() {}
inline bool PulseAvailable() { return true; }
inline PulseStatus GetPulseStatus() { return {}; }
inline PairCaptureStatus GetPairCaptureStatus() { return {}; }
inline bool RequestPairCapture(const char**, bool) { return false; }
inline Dx11Route::Report Dx11RouteReport() { return Dx11Route::Report::None; }
inline const char* NativeDx11SceneStatus() { return nullptr; }
inline SessionGuard::Verdict SessionGuardNotice() { return {}; }
inline void SessionGuardEnableAnyway() {}
inline const char* SessionGuardReasonText(const SessionGuard::Verdict&) { return ""; }
namespace GpuTiming
{
inline Snapshot snapshot;
inline Snapshot GetSnapshot() { return snapshot; }
inline void SetEnabled(bool) {}
} // namespace GpuTiming
namespace PresentBoundary
{
inline std::atomic_bool g_lastDeclined { false };
}
namespace ModelLog
{
inline int ConfigCount() { return running ? 2 : -1; }
inline ActiveWeightSet Active() { return {}; }
inline std::vector<std::string> ConfigNames() { return {}; }
} // namespace ModelLog
namespace ModelFile
{
inline bool Explicit() { return false; }
inline std::optional<std::filesystem::path> Find(const std::filesystem::path&) { return {}; }
} // namespace ModelFile
namespace NgxInfo
{
inline std::optional<std::filesystem::path> FindModel() { return {}; }
inline std::optional<Snapshot> Described(const std::filesystem::path&) { return {}; }
inline std::string DriverFromLoaderVersion(const uint16_t*) { return {}; }
} // namespace NgxInfo
namespace GameProfiles
{
inline std::string Summary() { return {}; }
} // namespace GameProfiles
namespace ReplayRecorder
{
inline Status GetStatus() { return {}; }
inline bool Request(const char**) { return false; }
} // namespace ReplayRecorder
namespace ExposureScan
{
inline std::vector<Candidate> Report() { return {}; }
inline const char* Status() { return ""; }
inline bool Scanning() { return false; }
inline Verdict Where() { return {}; }
inline float BestValue(int*, float*, float*) { return 0; }
inline std::string MatchLine() { return {}; }
inline std::vector<AnchorPoint> Anchors() { return {}; }
inline bool AnchorAdd(float, float) { return false; }
inline void AnchorRemove(int) {}
inline void AnchorSetWhite(int, float) {}
inline float AnchoredWhitePoint(float, bool, float) { return 1; }
inline std::string SerializeAnchors() { return {}; }
} // namespace ExposureScan
namespace SlNativeNr
{
inline constexpr char kMenuLine[] = "";
struct Fixture
{
    bool Active() const { return false; }
};
inline Fixture& Instance()
{
    static Fixture x;
    return x;
}
} // namespace SlNativeNr
} // namespace DlssNr
