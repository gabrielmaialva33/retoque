#pragma once

// Unreal Engine games ship nvngx_dlss.dll, nvngx_dlssd.dll and nvngx_dlssg.dll inside their NVIDIA plugins
// (<Root>/Engine/Plugins/Runtime/Nvidia/DLSS/Binaries/ThirdParty/Win64, <Root>/<Project>/Plugins/...). Startup looks
// beside OptiScaler and the exe and, through Util::FindFilePath, under the UE root when the exe sits in
// <Project>/Binaries/Win64; a game whose exe sits elsewhere (Phantom Blade Zero: <Root>/platform/steam) found none
// of them and lost DLSS. Upstream answers that one game with the ExtendedSearch quirk, a recursive scan for Plugins
// folders. This is the default for every game, used only for a file that search did not find: the nearest folder at
// most MaxRootLevels above the exe's that holds Engine, then fixed plugin folders below it, one directory listing
// deep, in a fixed order. Nothing is loaded. Same problem fixed in kkyleeb21's D18 3de7b799, janblade bacc36f9 and
// ShyVortex 27bcb0ee; no code taken. docs/linux-proton/unreal-dlss-search-20261030.md.

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace UnrealDlssSearch
{

namespace fs = std::filesystem;

// <Root>/<Project>/Binaries/Win64 is three folders below the root; one more for exes a level deeper
inline constexpr int MaxRootLevels = 4;

inline bool IsDirectory(const fs::path& path)
{
    std::error_code ec;
    return fs::is_directory(path, ec);
}

inline bool IsFile(const fs::path& path)
{
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// ASCII, case-insensitive: Windows folder names compare that way
inline bool StartsWith(const fs::path& name, std::string_view prefix)
{
    const auto& text = name.native();
    if (text.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i)
    {
        auto c = static_cast<unsigned long>(text[i]);
        auto p = static_cast<unsigned long>(static_cast<unsigned char>(prefix[i]));
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (p >= 'A' && p <= 'Z')
            p += 'a' - 'A';
        if (c != p)
            return false;
    }
    return true;
}

inline bool SameName(const fs::path& name, std::string_view other)
{
    return name.native().size() == other.size() && StartsWith(name, other);
}

// The exe's folder without the separator remove_filename() leaves, so it does not count as a level
inline fs::path Folder(const fs::path& exeDir)
{
    auto folder = exeDir.lexically_normal();
    if (!folder.has_filename() && folder.has_relative_path())
        folder = folder.parent_path();
    return folder;
}

// Nearest of the exe's folder and up to MaxRootLevels folders above it that holds an Engine folder
inline std::optional<fs::path> FindRoot(const fs::path& exeDir)
{
    auto folder = Folder(exeDir);
    for (int level = 0; level <= MaxRootLevels && !folder.empty(); ++level)
    {
        if (IsDirectory(folder / "Engine"))
            return folder;

        auto parent = folder.parent_path();
        if (parent == folder)
            break;
        folder = parent;
    }
    return std::nullopt;
}

// The game's project folder: the one holding the exe (<Root>/<Project>/Binaries/...), else the root's only folder
// besides Engine that has Binaries. Several projects and an exe outside all of them: none, rather than a guess.
inline std::optional<fs::path> FindProject(const fs::path& root, const fs::path& exeDir)
{
    std::error_code ec;
    const auto relative = Folder(exeDir).lexically_relative(root);
    auto part = relative.begin();
    if (!relative.empty() && part != relative.end() && *part != ".." && !SameName(*part, "Engine"))
    {
        const auto project = *part;
        if (++part != relative.end() && SameName(*part, "Binaries"))
            return root / project;
    }

    std::optional<fs::path> only;
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec))
    {
        const auto& folder = it->path();
        if (SameName(folder.filename(), "Engine") || !IsDirectory(folder) || !IsDirectory(folder / "Binaries"))
            continue;
        if (only.has_value())
            return std::nullopt;
        only = folder;
    }
    return only;
}

// Plugin folders below a project or Engine folder, in order: the immediate children of a fixed set of plugin
// containers whose names start with DLSS or Streamline (sorted), then NGX's own third-party folder
inline void AddPluginFolders(const fs::path& base, std::vector<fs::path>& folders)
{
    static constexpr std::string_view containers[] = { "Plugins/Runtime/Nvidia", "Plugins/Nvidia", "Plugins/Runtime",
                                                       "Plugins/Marketplace", "Plugins" };
    for (auto container : containers)
    {
        std::vector<fs::path> plugins;
        std::error_code ec;
        for (fs::directory_iterator it(base / fs::path(container), fs::directory_options::skip_permission_denied, ec),
             end;
             !ec && it != end; it.increment(ec))
        {
            const auto name = it->path().filename();
            if ((StartsWith(name, "DLSS") || StartsWith(name, "Streamline")) && IsDirectory(it->path()))
                plugins.push_back(it->path());
        }
        std::sort(plugins.begin(), plugins.end());
        for (const auto& plugin : plugins)
            folders.push_back(plugin / "Binaries" / "ThirdParty" / "Win64");
    }
    folders.push_back(base / "Binaries" / "ThirdParty" / "NVIDIA" / "NGX" / "Win64");
}

// Every folder to look in, in order. The project's plugins come first: a project plugin is the copy the game loads
// over an engine plugin of the same name.
inline std::vector<fs::path> CandidateFolders(const fs::path& exeDir)
{
    std::vector<fs::path> folders;
    const auto root = FindRoot(exeDir);
    if (!root.has_value())
        return folders;

    if (const auto project = FindProject(*root, exeDir))
        AddPluginFolders(*project, folders);
    AddPluginFolders(*root / "Engine", folders);

    for (auto& folder : folders)
        folder.make_preferred(); // native separators for the NGX path list and LoadLibrary
    return folders;
}

// First candidate folder holding fileName; no version comparison
inline std::optional<fs::path> Find(const fs::path& exeDir, const fs::path& fileName)
{
    for (const auto& folder : CandidateFolders(exeDir))
    {
        auto file = folder / fileName;
        if (IsFile(file))
            return file;
    }
    return std::nullopt;
}

} // namespace UnrealDlssSearch
