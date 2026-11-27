#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

// Adapted policy from shyvortex/OptiScaler (GPL-3.0), 1397ca97, DlssgPresetPolicy.h and its DRS answer.
// Only the process-local preset answer: no driver profile writes, loader or multiplier control.
namespace DlssgPresetPolicy
{
inline constexpr uint32_t kSettingId = 0x10E41DF1;
enum class Preset : uint32_t
{
    Auto,
    A,
    B,
    C
};
inline std::optional<Preset> Parse(std::string_view text)
{
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    if (text.size() == 4 && lower(text[0]) == 'a' && lower(text[1]) == 'u' && lower(text[2]) == 't' &&
        lower(text[3]) == 'o')
        return Preset::Auto;
    if (text.size() == 1 && lower(text[0]) >= 'a' && lower(text[0]) <= 'c')
        return static_cast<Preset>(lower(text[0]) - 'a' + 1);
    return std::nullopt;
}
inline const char* Name(Preset preset)
{
    switch (preset)
    {
    case Preset::A:
        return "a";
    case Preset::B:
        return "b";
    case Preset::C:
        return "c";
    default:
        return "auto";
    }
}
// File versions are numeric WORDs. Zero is unknown; no unbounded string-to-int parsing.
inline constexpr uint64_t Version(uint16_t major, uint16_t minor, uint16_t patch)
{
    return (uint64_t(major) << 32) | (uint64_t(minor) << 16) | patch;
}
inline bool Available(bool unlock, bool nativeDlssg, bool drsHook) { return unlock && nativeDlssg && drsHook; }
inline std::optional<uint32_t> Override(Preset preset, bool unlock, uint64_t runtime)
{
    if (!unlock || preset < Preset::A || preset > Preset::C || (preset == Preset::C && runtime < Version(310, 9, 16)))
        return std::nullopt;
    return static_cast<uint32_t>(preset);
}

// The adapter statically checks these NVAPI constants. Templates let host tests exercise the exact
// writes, including a missing-key answer, without Windows or the driver. A/B/C are the only values.
template <typename Setting>
int32_t Answer(int32_t result, uint32_t id, Setting* setting, uint32_t expectedVersion, std::optional<uint32_t> value)
{
    if (id != kSettingId || !value || *value < 1 || *value > 3 || !setting || setting->version != expectedVersion)
        return result;
    if (result == 0 && setting->settingType != 0)
        return result;
    if (result != 0 && result != -160)
        return result;
    if (result == -160)
    {
        *setting = {};
        setting->version = expectedVersion;
        setting->settingId = kSettingId;
        setting->settingType = static_cast<decltype(setting->settingType)>(0);
        constexpr char name[] = "Override DLSS-FG preset";
        for (size_t i = 0; i < sizeof(name); ++i)
            setting->settingName[i] = name[i];
    }
    setting->settingLocation = static_cast<decltype(setting->settingLocation)>(0);
    setting->isCurrentPredefined = 0;
    setting->u32CurrentValue = *value;
    return 0;
}
} // namespace DlssgPresetPolicy
