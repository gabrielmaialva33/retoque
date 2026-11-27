#pragma once

#include "DlssgPresetPolicy.h"
#include "MfgUnlock.h"
#include <Config.h>
#include <nvapi/NvApiHooks.h>
#include <NvApiDriverSettings.h>

namespace DlssgPresetPolicy
{
// Source: shyvortex/OptiScaler 1397ca97 (GPL-3.0). The saved driver profile is never written.
inline NvAPI_Status AnswerRead(NvAPI_Status result, NVDRS_SETTING* setting)
{
    static_assert(NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID == kSettingId);
    static_assert(NVAPI_OK == 0 && NVAPI_SETTING_NOT_FOUND == -160);
    static_assert(NVDRS_DWORD_TYPE == 0 && NVDRS_CURRENT_PROFILE_LOCATION == 0);
    // DLSS-G consumes this at feature creation; changing the menu is for the next launch.
    static const auto preset = Parse(Config::Instance()->FGDLSSGAdaMfgPreset.value_or_default()).value_or(Preset::Auto);
    const auto value = Override(preset, Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default(),
                                preset == Preset::C ? MfgUnlock::PresetRuntimeVersion() : 0);
    return static_cast<NvAPI_Status>(
        Answer(static_cast<int32_t>(result), kSettingId, setting, NVDRS_SETTING_VER, value));
}
} // namespace DlssgPresetPolicy
