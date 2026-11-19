#include <dlssnr/DlssNr_AdvisorRuntime.h>
#include <cassert>
#include <array>
#include <sstream>
#include <set>
#include <string>
#include <cstring>
namespace Localization
{
const char* Tr(const char* s) { return s; }
const char* Label(const char* s) { return s; }
} // namespace Localization
constexpr int ImGuiTableFlags_Borders = 1, ImGuiTableFlags_RowBg = 2;
namespace ImGui
{
std::set<std::string> shown;
std::string click, clipboard;
void SeparatorText(const char*) {}
template <class... T> void TextWrapped(const char*, T...) {}
template <class... T> void Text(const char*, T...) {}
bool Button(const char* s)
{
    shown.insert(s);
    return click == s;
}
bool SliderInt(const char*, int*, int, int) { return false; }
bool BeginTable(const char*, int, int) { return true; }
void TableSetupColumn(const char*) {}
void TableHeadersRow() {}
void TableNextRow() {}
void TableNextColumn() {}
void EndTable() {}
void SetClipboardText(const char* s) { clipboard = s; }
} // namespace ImGui
int configWrites = 0;
template <class T> struct Setting
{
    T value {};
    T value_or_default() const { return value; }
    Setting& operator=(T v)
    {
        value = v;
        ++configWrites;
        return *this;
    }
};
struct Config
{
    struct Timing
    {
        bool Enabled = true;
        unsigned Interval = 1;
    } timing;
    Timing GetDlssNrGpuTimingSettings() const { return timing; }
    bool DlssNrDx11SceneStageAtStart() const { return false; }
    Setting<bool> DlssNrEnabled { true }, DlssNrUseProxy, DlssNrCadenceAdaptive, DlssNrPeripheryCompression,
        DlssNrHoldFrame;
    Setting<unsigned> DlssNrStage, DlssNrHookMethod, DlssNrCadence { 1 };
    Setting<float> DlssNrWorkingScale { 1 };
};
Config config;
namespace DlssNr
{
float WorkingScale() { return config.DlssNrWorkingScale.value; }
} // namespace DlssNr

namespace sl
{
enum class DLSSGMode
{
    eOff
};
}
struct FakeFG
{
    bool IsActive() const { return false; }
    bool IsPaused() const { return false; }
};
struct State
{
    FakeFG* currentFG = nullptr;
    unsigned api = 0;
    sl::DLSSGMode dlssgLastSetMode = sl::DLSSGMode::eOff;
    unsigned DlssgDetectedInterpolationCount() const { return 0; }
    static State& Instance()
    {
        static State s;
        return s;
    }
};
