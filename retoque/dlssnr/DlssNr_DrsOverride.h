#pragma once

#include <atomic>
#include <cstdint>

// The driver's DLSS Neural Rendering overrides, as NvAPI_DRS_GetSetting reports them
// (docs/dlssnr/design/model-310-9-16.md, "Driver overrides").
//
// R610 added three NR settings to the driver profile; the NVIDIA App writes them on Windows, and dxvk-nvapi v0.9.3
// answers them on Proton from DXVK_NVAPI_DRS_* variables. The vendored external/nvapi predates them, so the ids and
// values are here, copied from NvApiDriverSettings.h at NVIDIA/nvapi 87dca625 (R615, the headers dxvk-nvapi v0.9.3
// builds with):
//
//   NGX_DLSS_NR_OVERRIDE                          0x10E41E04  0 off, 1 on
//   NGX_DLSS_NR_OVERRIDE_RENDER_PRESET_SELECTION  0x10E41DF8  0 off, 1-4 presets A-D, 0xFFFFFF latest
//   NGX_DLSS_NR_SL_OVERRIDE                       0x10E41E05  0 off, 1 on
//
// Retoque chooses the NR weight set itself ([DlssNr] Preset, sent as DLSSNR.Hint.Render.Preset), so with NR on the
// preset selection is reported to its reader as off, as NvApiHooks does for the SR and RR selections. The two
// switches are only reported: what turning them off would change (the driver's choice of model file) is not known.
// With NR off the hook never comes here. Standard library only, so tests/nr-drs-override compiles it as it is.

namespace DlssNr::DrsOverride
{
inline constexpr uint32_t kOverrideId = 0x10E41E04;
inline constexpr uint32_t kPresetSelectionId = 0x10E41DF8;
inline constexpr uint32_t kSlOverrideId = 0x10E41E05;
inline constexpr uint32_t kPresetSelectionOff = 0;
inline constexpr uint32_t kPresetSelectionLatest = 0x00FFFFFF;

constexpr int Slot(uint32_t settingId)
{
    switch (settingId)
    {
    case kOverrideId:
        return 0;
    case kPresetSelectionId:
        return 1;
    case kSlOverrideId:
        return 2;
    default:
        return -1;
    }
}

constexpr bool IsNrSetting(uint32_t settingId) { return Slot(settingId) >= 0; }

constexpr const char* Name(uint32_t settingId)
{
    switch (settingId)
    {
    case kOverrideId:
        return "NGX_DLSS_NR_OVERRIDE";
    case kPresetSelectionId:
        return "NGX_DLSS_NR_OVERRIDE_RENDER_PRESET_SELECTION";
    case kSlOverrideId:
        return "NGX_DLSS_NR_SL_OVERRIDE";
    default:
        return "?";
    }
}

constexpr const char* ValueName(uint32_t settingId, uint32_t value)
{
    if (settingId != kPresetSelectionId)
        return value == 0 ? "off" : value == 1 ? "on" : "unknown";

    switch (value)
    {
    case kPresetSelectionOff:
        return "off";
    case 1:
        return "preset A";
    case 2:
        return "preset B";
    case 3:
        return "preset C";
    case 4:
        return "preset D";
    case kPresetSelectionLatest:
        return "latest";
    default:
        return "unknown";
    }
}

struct Seen
{
    uint32_t driverValue = 0; // what the driver answered
    bool neutralised = false; // the reader is to see kPresetSelectionOff instead
    bool log = false;         // first read of this value for this setting
};

// One per process. Called by the DRS hook from whatever thread reads the setting, only with NR on, for an NR setting
// the driver answered as a DWORD (taken by value: NvU32 is unsigned long under MSVC, not uint32_t).
class Observer
{
  public:
    Seen Filter(uint32_t settingId, uint32_t driverValue)
    {
        Seen seen { driverValue };
        const int slot = Slot(settingId);
        if (slot < 0)
            return seen;

        // Logged at the first read and when the value changes: the bit above the 32-bit value marks "logged".
        const uint64_t mark = (uint64_t { 1 } << 32) | driverValue;
        seen.log = _logged[slot].exchange(mark, std::memory_order_relaxed) != mark;
        seen.neutralised = settingId == kPresetSelectionId && driverValue != kPresetSelectionOff;
        return seen;
    }

  private:
    std::atomic<uint64_t> _logged[3] {};
};

inline Observer& Driver()
{
    static Observer observer;
    return observer;
}
} // namespace DlssNr::DrsOverride
