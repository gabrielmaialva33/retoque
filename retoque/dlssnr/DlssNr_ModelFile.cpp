#include "pch.h"

#include "DlssNr_ModelFile.h"
#include "DlssNr_ModelFileFind.h"

#include <Config.h>
#include <Util.h>

#include <mutex>
#include <string>

// The glue of DlssNr_ModelFile.h: the setting Config froze at start-up, the default search where it always was,
// and the one-time lines. docs/dlssnr/design/model-file.md.
namespace DlssNr::ModelFile
{
namespace
{
std::mutex g_lock;             // the lines below and the resolved name; a leaf
std::string g_refusal;         // set once per process, from the frozen setting, never shrunk: c_str() stays valid
std::filesystem::path g_named; // the first named file found, for LoadedModule
std::string g_label;
bool g_saidFound = false;
bool g_saidRefused = false;

std::filesystem::path OptiFolder() { return Util::DllPath().remove_filename(); }
std::filesystem::path ExeFolder() { return Util::ExePath().remove_filename(); }
} // namespace

bool Explicit() { return !IsAuto(Config::Instance()->DlssNrModelFileAtStart()); }

std::optional<std::filesystem::path> Find(const std::filesystem::path& siteFolder)
{
    if (!Explicit())
    {
        // auto: the search each site has always made, from the folder it has always passed.
        auto snippet = Util::FindFilePath(siteFolder, kDefaultName);
        if (!snippet.has_value())
            snippet = Util::FindFilePath(ExeFolder(), kDefaultName);
        return snippet;
    }

    const auto opti = OptiFolder();
    const auto exe = ExeFolder();
    const Named named = Resolve(Config::Instance()->DlssNrModelFileAtStart(), opti, exe,
                                [](const std::filesystem::path& p)
                                {
                                    std::error_code ec;
                                    return std::filesystem::is_regular_file(p, ec) && !ec;
                                });

    std::string line;
    {
        std::lock_guard lock(g_lock);
        if (named.path.empty())
        {
            if (g_refusal.empty())
                g_refusal = named.refusal;
            if (!g_saidRefused)
            {
                g_saidRefused = true;
                line = "DLSS-NR model: " + named.refusal;
            }
        }
        else
        {
            // The first file found is the one the loaders load and keep for the process (they load once); a later
            // Find that lands elsewhere (a file moved meanwhile) does not change which module is ours.
            if (g_named.empty())
            {
                g_named = named.path;
                g_label = named.label;
            }
            if (!g_saidFound)
            {
                g_saidFound = true;
                line = "DLSS-NR model: [DlssNr] ModelFile names " + named.label +
                       "; Retoque's loaders use it (UseProxy cannot, and is refused while it is set)";
            }
        }
    }
    if (!line.empty())
    {
        if (named.path.empty())
            LOG_ERROR("{}", line);
        else
            LOG_INFO("{}", line);
    }

    if (named.path.empty())
        return std::nullopt;
    return named.path;
}

const char* Refusal()
{
    std::lock_guard lock(g_lock);
    return g_refusal.empty() ? "[DlssNr] ModelFile names a file that was not found" : g_refusal.c_str();
}

const char* NotFoundReason()
{
    return Explicit() ? Refusal() : "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
}

std::string Display(const std::filesystem::path& path)
{
    if (!Explicit())
        return path.string();
    return Label(path, OptiFolder(), ExeFolder());
}

HMODULE LoadedModule()
{
    if (!Explicit())
        return GetModuleHandleW(L"nvngx_dlssnr.dll");
    std::filesystem::path named;
    {
        std::lock_guard lock(g_lock);
        named = g_named;
    }
    // By its full path: the game may have its own nvngx_dlssnr.dll loaded under the same base name.
    return named.empty() ? nullptr : GetModuleHandleW(named.wstring().c_str());
}
} // namespace DlssNr::ModelFile
