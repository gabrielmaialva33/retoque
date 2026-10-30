# Unreal Engine plugin DLSS search (2026-10-30)

Not Linux-specific; filed with the other focused fork records. Code: `retoque/misc/UnrealDlssSearch.h`, called
from `OptiScaler/dllmain.cpp` after the existing searches.
Test: [unreal-dlss-search](../../tests/unreal-dlss-search/README.md).

## Problem

Startup fills `State::NVNGX_DLSS_Path`, `NVNGX_DLSSD_Path` and `NVNGX_DLSSG_Path` with `Util::FindFilePath` from
OptiScaler's folder, then the exe's. That function searches below the start folder and, when the exe sits in
`<Project>/Binaries/Win64` (or `WinGDK`, `Win64MasterMasterSteamPGO`), everything below the UE root. Unreal Engine
games ship the files in their NVIDIA plugins (`Engine/Plugins/Runtime/Nvidia/DLSS/Binaries/ThirdParty/Win64`,
`StreamlineCore/...` for `nvngx_dlssg.dll`, or the project's `Plugins`). An exe elsewhere (Phantom Blade Zero:
`<Root>/platform/steam`) reaches none of them; with no `nvngx_dlss.dll` startup logs "nvngx_dlss.dll not found,
disabling DLSS". Upstream (`f81d9d20`, `e7e3872d`) added the `ExtendedSearch` quirk for `pbz.exe` only: from up to
four ancestors, a recursive scan (depth 5) for folders named `Plugins`, then an unbounded search below each.

## Other forks (read, no code taken)

| Fork | Approach |
|---|---|
| kkyleeb21 D18 `3de7b799` | New `DlssFileDiscovery.h` replacing the startup calls: after OptiScaler/exe, nearest ancestor (4 levels) with `Engine`; fixed candidates `Engine/Plugins/Runtime/Nvidia/{DLSS,StreamlineCore}`, then the exe's project (or the root's single project, found by `Binaries` or `.uproject`) under `Plugins/Runtime/Nvidia`, `Plugins/Nvidia`, `Plugins` and `Binaries/ThirdParty/NVIDIA/NGX/Win64`. Engine before project. Host test on real folders. |
| janblade `bacc36f9` | Step 4 in `FindFilePath` for every game and every file name: nearest ancestor (4 levels) with `Engine/Plugins`, then the unbounded BFS below it. Also lets `NvngxDlssPath` name a folder. |
| ShyVortex `27bcb0ee` | `GameRootSearch::FindEngineRoot` (nearest ancestor within 3 levels holding `Engine`, start folder skipped), then the unbounded BFS of the whole root, only when the Win64 fallback did not run. Header unit test plus a source-text wiring check. |

All three target the Phantom Blade Zero layout. janblade and ShyVortex reuse the recursive BFS (bounded only by the
install's size); D18 alone uses fixed candidates.

## Design

- Runs in `dllmain.cpp` only for a file still missing after the existing searches, `NvngxDlssPath` and the
  `ExtendedSearch` quirk; a path already found is never touched. `Util::FindFilePath` and the quirk are unchanged.
- Root: the nearest of the exe's folder and up to four folders above it holding an `Engine` folder.
- Project: the root's child the exe is in when the next folder is `Binaries`; otherwise the root's only child (besides
  `Engine`) with a `Binaries` folder; several, none.
- Candidates, project first (UE loads a project plugin over an engine one of the same name), then `Engine`: the
  immediate children named `DLSS*` or `Streamline*` (ASCII case-insensitive, sorted) of `Plugins/Runtime/Nvidia`,
  `Plugins/Nvidia`, `Plugins/Runtime`, `Plugins/Marketplace`, `Plugins`, each `+ Binaries/ThirdParty/Win64`; then
  `Binaries/ThirdParty/NVIDIA/NGX/Win64`. At most a dozen one-level listings; no recursion.
- First regular file wins; versions are not compared (one copy is the normal case, and the order follows the copy the
  game itself loads). The chosen path is logged: `<file> found in an Unreal Engine plugin folder: <path>`.
- All filesystem calls use `error_code`; an unreadable folder is skipped. Nothing is loaded.

Effect: an Unreal game whose files were missed now keeps `DLSSEnabled` on an NVIDIA GPU, and NGX receives the
plugin folders in its feature path list as for a file the BFS found.

## Not verified

Host test only (ASan/UBSan on Linux, real folder trees). Not built with MSVC, not run in a game, under Wine or on
Windows. Needs an in-game check on an Unreal title whose exe is outside `<Project>/Binaries/Win64` without the quirk,
ideally Phantom Blade Zero with the quirk off, and a standard-layout UE game to confirm its log is unchanged.
