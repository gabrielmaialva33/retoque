#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

// Where Retoque's loaders find the NR model, with [DlssNr] ModelFile applied (DlssNr_ModelFile.h is the rule,
// DlssNr_ModelFile.cpp the glue). docs/dlssnr/design/model-file.md.
namespace DlssNr::ModelFile
{
// [DlssNr] ModelFile at start-up names a file (not auto).
bool Explicit();

// The model for a loader. auto: the search the caller has always made, from siteFolder (its own idea of where
// OptiScaler is), then beside the game. A named file: resolved beside OptiScaler, then the game, or as given when
// absolute; nullopt when it is not there, never the default.
std::optional<std::filesystem::path> Find(const std::filesystem::path& siteFolder);

// Why Find found nothing, for a route's reason: the default text under auto, the named file's otherwise. Valid
// for the process.
const char* NotFoundReason();

// The text a log line gives a model path: unchanged under auto; under a named file, the path relative to the folder
// it lies in, or its file name, so a folder of the user's own is not written out.
std::string Display(const std::filesystem::path& path);

// The model module Retoque loaded: by base name under auto, as before; by the named file's full path otherwise, so
// a game's own nvngx_dlssnr.dll is not taken for it. Null when it is not loaded.
HMODULE LoadedModule();
} // namespace DlssNr::ModelFile
