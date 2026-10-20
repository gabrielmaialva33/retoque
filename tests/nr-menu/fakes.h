#include <dlssnr/DlssNr_PassSettings.h>
#include <dlssnr/DlssNr_Cadence.h>
#include <algorithm>
#include <map>
#include <cassert>
#include <string>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <set>
#include <array>
#include <sstream>
#include <dlssnr/DlssNr_ModelLogParse.h>
#define IM_ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
using ImGuiID = size_t;
namespace Localization
{
const char* Tr(const char* x) { return x; }
const char* Label(const char* x) { return x; }
} // namespace Localization
namespace ImGui
{
std::vector<std::string> stack, shown;
std::map<std::string, double> actions;
std::string last;
bool edited = false;
// BeginDisabled/EndDisabled nest; a disabled control is drawn but a click on it changes nothing.
std::vector<bool> disabled;
std::set<std::string> shownDisabled;
void BeginDisabled(bool d = true) { disabled.push_back(d); }
void EndDisabled()
{
    assert(!disabled.empty());
    disabled.pop_back();
}
bool Disabled() { return std::find(disabled.begin(), disabled.end(), true) != disabled.end(); }
void PushID(const char* id) { stack.emplace_back(id); }
void PushID(int id) { stack.push_back(std::to_string(id)); }
void PopID()
{
    assert(!stack.empty());
    stack.pop_back();
}
std::string Key(const char* label)
{
    std::string key;
    for (auto& x : stack)
        key += x + '/';
    return key + label;
}
ImGuiID GetID(const char* label) { return std::hash<std::string> {}(Key(label)); }
template <class T> bool Input(const char* label, T* value)
{
    last = Key(label);
    shown.push_back(last);
    edited = false;
    auto item = actions.find(last);
    if (item == actions.end())
        return false;
    if (Disabled())
    {
        shownDisabled.insert(last);
        actions.erase(item);
        return false;
    }
    *value = static_cast<T>(item->second);
    actions.erase(item);
    return edited = true;
}
bool Checkbox(const char* label, bool* x) { return Input(label, x); }
bool SliderFloat(const char* label, float* x, float, float, const char*) { return Input(label, x); }
bool SliderInt(const char* label, int* x, int, int) { return Input(label, x); }
bool Combo(const char* label, int* x, const char* const*, int) { return Input(label, x); }
bool SmallButton(const char* label)
{
    bool x = false;
    return Input(label, &x);
}
bool IsItemDeactivatedAfterEdit() { return edited; }
bool IsItemActive() { return false; }
void SameLine() {}
std::vector<std::string> sections;
void SeparatorText(const char* x) { sections.emplace_back(x); }
int widths = 0;
void PushItemWidth(float) { ++widths; }
void PopItemWidth()
{
    assert(widths > 0);
    --widths;
}
bool IsItemHovered() { return false; }
void BeginTooltip() {}
void EndTooltip() {}
void PushTextWrapPos(float) {}
void PopTextWrapPos() {}
float GetFontSize() { return 12.0f; }
void TextUnformatted(const char*) {}
template <class... T> void Text(const char*, T...) {}
template <class... T> void TextWrapped(const char* f, T...) { shown.emplace_back(f); }
std::vector<std::string> texts;
template <class... T> void TextDisabled(const char* f, T... v)
{
    std::ostringstream s;
    s << f;
    ((s << "|" << v), ...);
    texts.push_back(s.str());
}
void Reset()
{
    assert(stack.empty() && disabled.empty() && widths == 0);
    sections.clear();
    shown.clear();
    shownDisabled.clear();
    actions.clear();
    texts.clear();
    edited = false;
}
bool Saw(const std::string& id) { return std::find(shown.begin(), shown.end(), id) != shown.end(); }
} // namespace ImGui
struct OptionalBool
{
    bool value = false;
    bool value_or_default() const { return value; }
    OptionalBool& operator=(bool v)
    {
        value = v;
        return *this;
    }
};
struct OptionalU32
{
    uint32_t value = 1;
    uint32_t value_or_default() const { return value; }
    OptionalU32& operator=(uint32_t v)
    {
        value = v;
        return *this;
    }
};
struct OptionalFloat
{
    float value = 1.0f;
    float value_or_default() const { return value; }
    OptionalFloat& operator=(float v)
    {
        value = v;
        return *this;
    }
};
struct Config
{
    OptionalBool DlssNrShowAllSettings;
    OptionalFloat DlssNrTransferStrength;
    OptionalU32 DlssNrPreset;
    struct Timing
    {
        bool Enabled = false;
        unsigned Interval = 1;
    } timing;
    Timing GetDlssNrGpuTimingSettings() const { return timing; }
    void SetDlssNrMasterSetting(OptionalU32 Config::*, uint32_t v)
    {
        master.Preset = v;
        ++writes;
    }
    OptionalBool DlssNrUseProxy;
    OptionalU32 DlssNrCadence;
    OptionalBool DlssNrCadenceWithFrameGen;
    OptionalBool DlssNrCadenceSmoothing;
    OptionalBool DlssNrCadenceAdaptive;
    DlssNrResolvedPassSettings master;
    DlssNr::PassConfig::Overrides overrides;
    DlssNrPassSnapshot snapshot;
    unsigned writes = 0;
    DlssNrPassSnapshot GetDlssNrPassSnapshot() const { return snapshot; }
    DlssNrResolvedPassSettings GetDlssNrMasterSettings() const { return master; }
    DlssNrPassSettings GetDlssNrPassOverrides(uint32_t i) const { return DlssNr::PassConfig::Get(overrides, i); }
    void SetDlssNrPassCount(uint32_t n)
    {
        snapshot.Count = DlssNr::PassConfig::BoundCount(n);
        ++writes;
    }
    void SetDlssNrIndividualPassSettings(bool x)
    {
        snapshot.Individual = x;
        ++writes;
    }
    void SetDlssNrPassOverrides(uint32_t i, DlssNrPassSettings x)
    {
        DlssNr::PassConfig::Set(overrides, i, x);
        ++writes;
    }
};
namespace DlssNr::ModelLog
{
int configCount = -1; // what the model has said about its weight sets; -1 until it has
int ConfigCount() { return configCount; }
struct ActiveSet
{
    std::string config;
    int preset = -1;
};
ActiveSet active;
ActiveSet Active() { return active; }
std::vector<std::string> ConfigNames() { return {}; }
} // namespace DlssNr::ModelLog
namespace DlssNr
{
void HelpMarker(const char*) {}
unsigned ActivePassCount() { return 0; }
const char* MultipassStatus() { return "Restart required"; }
bool cadenceAvailable = true;
bool CadenceAvailable() { return cadenceAvailable; }
const char* CadenceStatus() { return DlssNr::Cadence::Describe(DlssNr::Cadence::Reason::Carried, 2); }
} // namespace DlssNr
