#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

// Which NR model file Retoque's own loaders use: [DlssNr] ModelFile (docs/dlssnr/design/model-file.md, tested by
// tests/nr-model-file).
//
// - auto (the default, also empty): the search every loader has always made, unchanged and made where it was made:
//   nvngx_dlssnr.dll through Util::FindFilePath beside OptiScaler, then beside the game. This header does not
//   reimplement it; DlssNr_ModelFile.cpp calls it.
// - a file name or a relative path: tried as a regular file beside OptiScaler, then beside the game's exe.
// - an absolute path: used as it is.
// A named file that is not there makes NR unavailable with a reason that names it. There is no fallback to the
// default: a silent fallback would make an A/B run compare a model with itself.
//
// The setting is the value at start-up (Config freezes it at its first load); a change during the session applies
// at the next start. The forwarder and the direct runtime load the path they are given, once per process.
//
// Only Retoque's loaders follow it. The driver's own route (UseProxy) loads whatever the driver finds and cannot be
// told a file; with a named file it is refused rather than run on another model.
//
// Standard library only: the filesystem query is injected, so the host test drives it.
namespace DlssNr::ModelFile
{
inline constexpr const char* kDefaultName = "nvngx_dlssnr.dll";

inline std::string Trimmed(std::string_view value)
{
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return std::string(value);
}

// auto, empty or blank: the default search.
inline bool IsAuto(std::string_view setting)
{
    std::string value = Trimmed(setting);
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value.empty() || value == "auto";
}

// The INI is UTF-8: a path is made from its bytes as UTF-8, never through the process's code page, and a label is
// UTF-8 for the log.
inline std::filesystem::path FromUtf8(std::string_view text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::string Utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// How a value is rooted, read off its text as Windows reads it (the platform the loaders run on, natively or under
// Wine), whatever the host: "C:\x" or "C:/x" and "\\server\share\x" are full paths; "C:x" (the drive's current
// folder) and "\x" (the current drive's root) depend on the process's working directory and are refused.
enum class Rooting : uint8_t
{
    Relative,
    Full,
    Partial,
};

inline Rooting RootingOf(std::string_view value)
{
    const auto slash = [](char c) { return c == '/' || c == '\\'; };
    const bool drive = value.size() >= 2 && std::isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':';
    if (drive)
        return value.size() >= 3 && slash(value[2]) ? Rooting::Full : Rooting::Partial;
    if (!value.empty() && slash(value[0]))
        return value.size() >= 2 && slash(value[1]) ? Rooting::Full : Rooting::Partial;
    return Rooting::Relative;
}

// What a named file resolved to. `path` is empty when it was not found; `label` is what the log says, never a
// folder outside the two this searches.
struct Named
{
    std::filesystem::path path;
    std::string label;
    std::string refusal;
};

// `inside`: whether `path` lies inside `folder` without leaving it (no "..").
inline bool Inside(const std::filesystem::path& path, const std::filesystem::path& folder)
{
    if (folder.empty())
        return false;
    const auto relative = path.lexically_normal().lexically_relative(folder.lexically_normal());
    if (relative.empty() || relative.is_absolute() || relative == ".")
        return false;
    for (const auto& part : relative)
    {
        if (part == "..")
            return false;
    }
    return true;
}

// The log's name for a resolved file: relative to the folder it lies in, or its file name alone.
inline std::string Label(const std::filesystem::path& path, const std::filesystem::path& optiFolder,
                         const std::filesystem::path& exeFolder)
{
    for (const auto* folder : { &optiFolder, &exeFolder })
    {
        if (Inside(path, *folder))
            return Utf8(path.lexically_normal().lexically_relative(folder->lexically_normal()));
    }
    return Utf8(path.filename());
}

// A named file (not auto). isRegularFile answers for a path whether it is a regular file (a directory named
// something.dll is not a model).
inline Named Resolve(std::string_view setting, const std::filesystem::path& optiFolder,
                     const std::filesystem::path& exeFolder,
                     const std::function<bool(const std::filesystem::path&)>& isRegularFile)
{
    Named named;
    std::string value = Trimmed(setting);
    const Rooting rooting = RootingOf(value);
    std::ranges::replace(value, '\\', '/');
    const std::filesystem::path wanted = FromUtf8(value).make_preferred();

    if (rooting == Rooting::Partial)
    {
        named.label = Utf8(wanted.filename());
        named.refusal = "[DlssNr] ModelFile names " + named.label +
                        " with a drive or root but not a full path, which would depend on the working directory";
        return named;
    }

    if (rooting == Rooting::Full)
    {
        named.label = Utf8(wanted.filename());
        if (isRegularFile(wanted))
            named.path = wanted;
        else
            named.refusal = "[DlssNr] ModelFile names " + named.label + ", which is not a file at the path given";
        return named;
    }

    for (const auto* folder : { &optiFolder, &exeFolder })
    {
        if (folder->empty())
            continue;
        const auto candidate = (*folder / wanted).lexically_normal();
        if (isRegularFile(candidate))
        {
            named.path = candidate;
            named.label = Label(candidate, optiFolder, exeFolder);
            return named;
        }
    }
    named.label = Utf8(wanted.filename());
    named.refusal = "[DlssNr] ModelFile names " + named.label +
                    ", which is not beside OptiScaler or the game; NR does not fall back to " + kDefaultName;
    return named;
}
} // namespace DlssNr::ModelFile
