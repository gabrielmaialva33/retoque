#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The keys a game needs before NR can run in it at all, applied without the user's ini
// (docs/dlssnr/design/game-profiles.md). Header-only and free of <windows.h>, so tests/nr-game-profiles runs it as
// shipped; DlssNr_GameProfiles.cpp applies it to Config.
//
// Only keys that decide whether the pass runs, each measured in its game here: never NR's on/off, never a look,
// frame-generation or LSFG preference. Every value goes in through set_volatile_value and only where the key has no
// value, so the user's ini always wins and a menu save never writes a profile's value back.
namespace DlssNr::GameProfiles
{
struct Profile
{
    const char* name;
    std::vector<std::string_view> exes;    // lowercase file names
    std::optional<unsigned> hookMethod;    // [DlssNr] HookMethod
    std::optional<bool> zeroGuideReset;    // [DlssNr] ZeroGuideReset
    std::optional<bool> vulkanPresentHost; // [DlssNr] VulkanPresentHost
    const char* dx11Upscaler = nullptr;    // [Upscalers] Dx11Upscaler, as the ini spells it
    const char* evidence = "";             // the design note that measured it
};

inline const std::vector<Profile>& Table()
{
    static const std::vector<Profile> table = {
        { "Divinity: Original Sin 2", { "eocapp.exe" }, 2u, {}, {}, nullptr, "nr-dx11-bridge-host.md" },
        { "Generation Zero", { "generationzero_f.exe" }, 2u, {}, {}, nullptr, "nr-dx11-bridge-host.md" },
        { "Sons Of The Forest", { "sonsoftheforest.exe" }, 2u, {}, {}, "dlss", "dx11-native-dlss-guides.md" },
        { "Hellraiser: Revival",
          { "hellraiser-win64-shipping.exe" },
          2u,
          true,
          {},
          nullptr,
          "hellraiser-video-present.md" },
        { "DOOM Eternal", { "doometernalx64vk.exe" }, {}, {}, true, nullptr, "nr-vulkan-present-host.md" },
        { "PCSX2", { "pcsx2-qt.exe" }, 2u, {}, {}, nullptr, "README.md, Status" },
    };
    return table;
}

inline std::string Lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// The profile for this process's executable file name (no directory), or none.
inline const Profile* Match(std::string_view exeName)
{
    const std::string lower = Lower(exeName);

    for (const auto& profile : Table())
    {
        for (const auto exe : profile.exes)
        {
            if (lower == exe)
                return &profile;
        }
    }

    return nullptr;
}

// One key the profile set, or left to the user because their ini already had it.
struct KeyResult
{
    std::string key;
    std::string value;
    bool applied = false;
};

template <typename Option, typename T>
inline void SetIfUnset(std::vector<KeyResult>& out, const char* key, std::string value, Option& option, const T& v)
{
    if (option.has_value())
    {
        out.push_back({ key, std::move(value), false });
        return;
    }

    option.set_volatile_value(v);
    out.push_back({ key, std::move(value), true });
}

// Applies a matched profile to the four options it may touch. `toUpscaler` turns the ini spelling of an upscaler
// into the option's value type (CodeToUpscaler in production).
template <typename HookOption, typename BoolOption, typename UpscalerOption, typename ToUpscaler>
inline std::vector<KeyResult> ApplyProfile(const Profile& profile, HookOption& hookMethod, BoolOption& zeroGuideReset,
                                           BoolOption& vulkanPresentHost, UpscalerOption& dx11Upscaler,
                                           ToUpscaler toUpscaler)
{
    std::vector<KeyResult> out;

    if (profile.hookMethod)
        SetIfUnset(out, "HookMethod", std::to_string(*profile.hookMethod), hookMethod, *profile.hookMethod);
    if (profile.zeroGuideReset)
        SetIfUnset(out, "ZeroGuideReset", *profile.zeroGuideReset ? "true" : "false", zeroGuideReset,
                   *profile.zeroGuideReset);
    if (profile.vulkanPresentHost)
        SetIfUnset(out, "VulkanPresentHost", *profile.vulkanPresentHost ? "true" : "false", vulkanPresentHost,
                   *profile.vulkanPresentHost);
    if (profile.dx11Upscaler != nullptr)
        SetIfUnset(out, "Dx11Upscaler", profile.dx11Upscaler, dx11Upscaler, toUpscaler(profile.dx11Upscaler));

    return out;
}

// Proton's own nvapi64.dll, for a prefix Proton gave none (it deletes the file for titles on its disablenvapi list,
// Divinity: Original Sin 2 among them). The Proton script exports WINEDLLPATH as
// "<proton>/files/lib/vkd3d:<proton>/files/lib/wine[:...]"; the DLL is <entry>/nvapi/x86_64-windows/nvapi64.dll under
// the entry that ends in "/wine". Returns the Windows path (Z: is the Unix root under Wine) of the first entry whose
// file `exists` accepts, or none.
template <typename Exists>
inline std::optional<std::string> FindProtonNvapi(std::string_view winedllpath, Exists exists)
{
    size_t start = 0;

    while (start <= winedllpath.size())
    {
        size_t end = winedllpath.find(':', start);
        if (end == std::string_view::npos)
            end = winedllpath.size();

        std::string_view entry = winedllpath.substr(start, end - start);
        while (entry.size() > 1 && entry.back() == '/')
            entry.remove_suffix(1);

        constexpr std::string_view kWine = "/wine";
        if (!entry.empty() && entry.front() == '/' && entry.size() > kWine.size() &&
            entry.substr(entry.size() - kWine.size()) == kWine)
        {
            std::string path = "Z:";
            for (const char c : entry)
                path += c == '/' ? '\\' : c;
            path += "\\nvapi\\x86_64-windows\\nvapi64.dll";

            if (exists(path))
                return path;
        }

        start = end + 1;
    }

    return std::nullopt;
}

// Production entry points, in DlssNr_GameProfiles.cpp. Apply runs once, from CheckQuirks, after upstream's own game
// configs and before anything loads NVAPI or creates a swapchain. Summary is what it did, for the menu; empty when
// it did nothing.
void Apply();
std::string Summary();
} // namespace DlssNr::GameProfiles
