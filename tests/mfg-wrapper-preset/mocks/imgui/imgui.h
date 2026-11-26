#pragma once
#include <string>
constexpr int ImGuiSelectableFlags_None = 0, ImGuiSelectableFlags_Disabled = 1;
namespace ImGui
{
inline unsigned combos = 0;
inline bool cDisabled = false;
inline int choose = -1;
inline bool BeginCombo(const char*, const char*)
{
    ++combos;
    return true;
}
inline bool Selectable(const char* label, bool, int flags)
{
    const bool c = std::string(label) == "Preset C";
    if (c)
        cDisabled = (flags == ImGuiSelectableFlags_Disabled);
    return choose == 3 && c && !cDisabled;
}
inline void EndCombo() {}
inline void TextWrapped(const char*) {}
} // namespace ImGui
