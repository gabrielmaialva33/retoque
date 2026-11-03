# Choosing the NR model file: [DlssNr] ModelFile

Date: 2026-11-03. Status: implemented, host-tested; not run in a game.
The idea comes from jeffron256/OptiScaler_DLSSNR `dad981d5` (GPL-3.0). That fork discovers renamed copies of the
model and a `DlssNrModels/<name>/` folder, and switches between them live. No code was taken from it. An
independent comparison (2026-11-03) recommended adapting selection only, and not its live switch: the switch
needs a lifetime contract that our forwarder, direct runtime and Vulkan pins do not have.

## Why

Comparing two model versions today means renaming files in the game folder between runs. With a setting that
names the file, an A/B run is reproducible from the INI.

## What it does

`retoque/dlssnr/DlssNr_ModelFile.h` holds the rule, using the standard library only. `DlssNr_ModelFile.cpp` is the
glue and `DlssNr_ModelFileFind.h` its interface.

- **`auto`** (the default; empty and blank count as auto): every loader makes the search it always made, from the
  folder it always passed: `Util::FindFilePath(<its folder>, "nvngx_dlssnr.dll")`, then beside the game. The
  resolver calls that search and does not reimplement it. It keeps its recursion, its filters, its quirk
  fallbacks and its reason text.
- **A file name or relative path** (`nvngx_dlssnr_310.8.dll`, `DlssNrModels/310.8/nvngx_dlssnr.dll`): it must be
  a regular file beside OptiScaler, then beside the game's exe. Backslashes are accepted. It is never tried
  against the working directory.
- **A full path** is used as it is. That means a drive and a slash (`D:\models\...`) or a UNC share
  (`\\server\share\...`). A value with a drive but no slash (`C:model.dll`) or a single leading slash
  (`\model.dll`) is refused: Windows resolves both against the working directory. The rule reads the text as
  Windows does, on any host.
- The value is UTF-8, as the INI is. The path is made from those bytes and never through the process's code
  page, and the labels in the log are UTF-8.
- **A named file that is not there** makes NR unavailable, with a reason that names the file. It **never falls
  back** to `nvngx_dlssnr.dll`: a silent fallback would make an A/B run compare a model with itself.

The setting is read once. Config's constructor freezes it after its first load (`DlssNrModelFileAtStart`), before
any route runs; if that load failed, the default is frozen. A change made later in the session (INI edit, reload,
save) applies at the next start. Each loader resolves the
frozen value when it loads. The forwarder and the direct runtime receive the path and load it once per process,
as before.

The value is read without `readString`, so it is not written into the log's list of parameters. Under a named
file, the log lines that name the model show it relative to the folder it lies in, or by file name only. This
covers NgxInfo's model line, the timing model-file line and the direct runtime's load error. Under `auto` those
lines are unchanged.

## The driver's own route (UseProxy)

`UseProxy` asks the driver to create feature 18, and the driver loads the model it finds itself; it cannot be told
a file. Its known-fault gate (`NgxInfo::FindModel`, `ngx-driver-616.md`) therefore keeps judging the file the
driver would find, whatever `ModelFile` says. With a named file, `UseProxy` is refused, with a line saying why.
Otherwise it would run the default model under a setting that names another one.

## Which loaded module is ours

Two places identified the loaded model by base name: the NgxInfo model line and the timing identity. A game can
have its own `nvngx_dlssnr.dll` loaded under the same base name, and `GetModuleHandleW` then picks either. Under a
named file, both now look the module up by the named file's full path. Under `auto` they look it up by base name,
as before.

## Install and release tools

The deploy script, the Windows installer, the packager and the release manifest already refused to copy or ship
`nvngx_dlssnr.dll`. They now treat the copies this note tells users to make the same way: any `nvngx_dlssnr*.dll`
and any path through a `DlssNrModels` folder.

- They are never copied or shipped.
- The deploy script's `--uninstall` never removes, restores or renames one, even when a receipt written before
  this change lists it. The Windows installer already left the model alone on uninstall.
- A model under another name outside `DlssNrModels` is allowed by `ModelFile` but not recognised by these
  filters. Keep such a copy out of the release folder.

The two "model missing" messages mention `ModelFile`.

## Not in this change

- Switching the model live, or keeping several models resident.
- A menu list of the models found. With two folders, two entries could name the same relative file and run the
  same copy. Selection is by INI only.
- Any change to the D3D11 probe's private copy (`nvngx.dll_dlssnr_d3d11probe.dll`). It now copies the chosen file,
  as before from the found one.
- Any change to which model the driver loads for the game's own features.

## Tests

`tests/nr-model-file`:

- The rule on a fake filesystem, with eleven cases:
  - auto and its spellings;
  - the folder order;
  - a missing file that is refused and never replaced by the default;
  - full paths (drive and UNC) and labels that never write out a folder of the user's;
  - partial roots (`C:x`, `\x`), which are refused;
  - UTF-8 names;
  - whitespace;
  - a directory named like a model;
  - no working-directory search;
  - containment.
- Twelve rule mutants.
- Source guards with eleven mutants:
  - only the resolver and the driver route search by the literal name;
  - only the resolver looks the module up by base name;
  - auto is the old search, from each site's own folder;
  - every loader and the pair capture ask the resolver;
  - UseProxy keeps the driver's file and refuses a named one;
  - identity and labels follow the named file, and a named model is looked up by its full path;
  - paths are made from UTF-8 (a host on Linux cannot show the code-page difference, so this is a guard);
  - the four config points, frozen by the constructor and kept out of the parameter list.
- `deploy-safety`, `windows-installer` and `release-evidence` gained renamed-copy and `DlssNrModels` cases.
  `deploy-safety` also has an uninstall over an older receipt that lists model copies as files, backups and
  renames.

Not proved: that a renamed model loads and runs through each loader (forwarder, direct, Vulkan), that the
model's own caller checks accept it, or anything on native Windows.
