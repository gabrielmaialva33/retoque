#pragma once
#include <dlssnr/DlssNr_Dx11Route.h>
#include <dlssnr/DlssNr_DriverGate.h>
#include <dlssnr/DlssNr_ModelFile.h>

template <typename T> struct Setting
{
    T value;
    T value_or_default() const { return value; }
};
struct Config
{
    Setting<bool> DlssNrEnabled { false }, DlssNrApplyModel { true };
    Setting<uint32_t> DlssNrPasses { 3 }, DlssNrCadence { 2 };
    std::string model = " auto ";
    const std::string& DlssNrModelFileAtStart() const { return model; }
};
namespace DlssNr
{
inline int reads = 0;
inline bool vkRunning = false, dxRunning = false;
inline Dx11Route::Report dxRoute = Dx11Route::Report::None;
inline const char* sceneStatus = nullptr;
inline bool IsRunningVk()
{
    ++reads;
    return vkRunning;
}
inline bool IsRunning()
{
    ++reads;
    return dxRunning;
}
inline const char* FailureReason()
{
    ++reads;
    return "test failure";
}
inline const char* HookStatus()
{
    ++reads;
    return "upscaled: active";
}
inline Dx11Route::Report Dx11RouteReport()
{
    ++reads;
    return dxRoute;
}
inline const char* NativeDx11SceneStatus()
{
    ++reads;
    return sceneStatus;
}
inline float WorkingScale()
{
    ++reads;
    return 0.5f;
}
inline float RRWorkingScale()
{
    ++reads;
    return 0.75f;
}
inline const char* CadenceStatus()
{
    ++reads;
    return "cadence declined";
}
} // namespace DlssNr
constexpr int ImGuiTableFlags_SizingFixedFit = 1, ImGuiTableColumnFlags_WidthStretch = 2;
namespace ImGui
{
inline bool table = true;
inline int ends = 0;
inline std::vector<std::string> values;
inline bool BeginTable(const char*, int, int) { return table; }
inline void TableSetupColumn(const char*, int = 0) {}
inline void TableNextColumn() {}
inline void TextDisabled(const char*, const char*) {}
inline void TextWrapped(const char*, const char* text) { values.emplace_back(text); }
inline void EndTable() { ++ends; }
} // namespace ImGui
