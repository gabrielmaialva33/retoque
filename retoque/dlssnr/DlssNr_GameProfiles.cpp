#include "pch.h"

#include "DlssNr_GameProfiles.h"

#include <Config.h>
#include <Logger.h>
#include <OptiTypes.h>
#include <State.h>
#include <SysUtils.h>
#include <Util.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace DlssNr::GameProfiles
{
namespace
{
std::mutex g_summaryMutex;
std::string g_summary;

std::string Describe(const std::vector<KeyResult>& results)
{
    std::string text;

    for (const auto& r : results)
    {
        if (!text.empty())
            text += ", ";
        text += r.key + "=" + r.value + (r.applied ? " (from the profile)" : " (left to your ini, which sets it)");
    }

    return text;
}

// Proton's nvapi64.dll when the prefix has none. Under Wine only, and only when the user gave no NvapiPath.
std::string FindNvapi()
{
    auto* cfg = Config::Instance();

    if (!State::Instance().isRunningOnLinux || cfg->NvapiDllPath.has_value())
        return {};

    wchar_t system[MAX_PATH] {};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    std::error_code ec;
    if (length == 0 || length >= MAX_PATH ||
        std::filesystem::exists(std::filesystem::path(system) / L"nvapi64.dll", ec))
        return {};

    std::string paths(32768, '\0');
    const DWORD got = GetEnvironmentVariableA("WINEDLLPATH", paths.data(), static_cast<DWORD>(paths.size()));
    if (got == 0 || got >= paths.size())
    {
        LOG_INFO("DLSS-NR game profile: the prefix has no nvapi64.dll and WINEDLLPATH does not say where Proton "
                 "keeps one; set [Libraries] NvapiPath if NGX refuses the model");
        return {};
    }
    paths.resize(got);

    const auto found = FindProtonNvapi(paths,
                                       [](const std::string& path)
                                       {
                                           std::error_code exists;
                                           return std::filesystem::is_regular_file(string_to_wstring(path), exists);
                                       });
    if (!found)
    {
        LOG_INFO("DLSS-NR game profile: the prefix has no nvapi64.dll and none was found under WINEDLLPATH ({})",
                 paths);
        return {};
    }

    cfg->NvapiDllPath.set_volatile_value(string_to_wstring(*found));
    return *found;
}
} // namespace

void Apply()
{
    auto* cfg = Config::Instance();

    // Default-identical when off: with NR off, nothing here touches any setting.
    if (!cfg->DlssNrEnabled.value_or_default() || !cfg->DlssNrGameProfiles.value_or_default())
        return;

    std::string summary;
    const std::string exe = wstring_to_string(Util::ExePath().filename().wstring());

    if (const Profile* profile = Match(exe))
    {
        const auto results =
            ApplyProfile(*profile, cfg->DlssNrHookMethod, cfg->DlssNrZeroGuideReset, cfg->DlssNrVulkanPresentHost,
                         cfg->Dx11Upscaler, [](const char* code) { return CodeToUpscaler(code); });
        const std::string keys = Describe(results);
        LOG_INFO("DLSS-NR game profile: {} ({}): {} [{}]", profile->name, exe, keys, profile->evidence);
        summary = std::string(profile->name) + ": " + keys;
    }

    if (const std::string nvapi = FindNvapi(); !nvapi.empty())
    {
        LOG_INFO("DLSS-NR game profile: the prefix has no nvapi64.dll; NvapiPath={} (found in Proton, this session "
                 "only)",
                 nvapi);
        summary += std::string(summary.empty() ? "" : "; ") + "NvapiPath from Proton";
    }

    std::lock_guard lock(g_summaryMutex);
    g_summary = std::move(summary);
}

std::string Summary()
{
    std::lock_guard lock(g_summaryMutex);
    return g_summary;
}
} // namespace DlssNr::GameProfiles
