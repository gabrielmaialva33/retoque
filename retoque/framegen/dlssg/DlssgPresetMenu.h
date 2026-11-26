#pragma once

#include "DlssgPresetPolicy.h"
#include "MfgUnlock.h"
#include <Config.h>
#include <imgui/imgui.h>
#include <nvapi/NvApiHooks.h>

namespace DlssgPresetPolicy
{
// A setting for a real native DLSS-G route only. Replacement FSR/XeFG and absent DRS expose no control.
inline void RenderMenu(bool nativeDlssg)
{
    auto config = Config::Instance();
    if (!Available(config->FGDLSSGAdaMfgUnlock.value_or_default(), nativeDlssg && MfgUnlock::PresetProviderObserved(),
                   NvApiHooks::o_NvAPI_DRS_GetSetting != nullptr))
        return;
    const auto preset = Parse(config->FGDLSSGAdaMfgPreset.value_or_default()).value_or(Preset::Auto);
    const char* labels[] = { "Auto (game / driver)", "Preset A", "Preset B", "Preset C" };
    if (ImGui::BeginCombo("DLSS-G preset", labels[static_cast<unsigned>(preset)]))
    {
        for (unsigned i = 0; i < 4; ++i)
        {
            const bool supported = i != 3 || MfgUnlock::PresetRuntimeVersion() >= Version(310, 9, 16);
            if (ImGui::Selectable(labels[i], i == static_cast<unsigned>(preset),
                                  supported ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled))
                config->FGDLSSGAdaMfgPreset = std::string(Name(static_cast<Preset>(i)));
        }
        ImGui::EndCombo();
    }
    ImGui::TextWrapped("Save settings and restart to apply the preset. The saved NVIDIA profile is unchanged.");
    if (preset == Preset::C && MfgUnlock::PresetRuntimeVersion() < Version(310, 9, 16))
        ImGui::TextWrapped(
            "Preset C needs DLSS-G 310.9.16 or newer; an old or unknown provider keeps the driver answer.");
}
} // namespace DlssgPresetPolicy
