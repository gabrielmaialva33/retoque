# tools/

Scripts this fork owns. Upstream's own scripts (`setup_linux.sh`, `setup_windows.bat`,
`OptiScaler/shaders/shader_tools/`) stay where upstream put them.

Every script derives the repo root from its own location, so each runs from any cwd.

## build-local.sh

Builds OptiScaler and the DLSS-NR forwarder on Linux with msvc-wine (MSVC under Wine).
The shell entry delegates to `build_local.py`, which takes the shared compiler-prefix
lock before changing build stamps or starting either project.

```bash
tools/build-local.sh [Release|Debug] [extra msbuild args]
```

Outputs go to `x64/<Config>/` and `x64/out/`. `./build-local.sh` at the repo root is a stub that
`exec`s this script. Call either entry directly; do not wrap it in another `flock` on
`msvc-prefix.lock`. A busy lock returns 75 without changing stamps or invoking the compiler.

Each stage has an owned process session. The Linux supervisor records process identities,
signals them through pidfds, and retains logs under `x64/logs/<date-pid-id>/`. Ninety seconds
without build-workload CPU or log growth, or a two-hour hard limit, returns 124 after bounded
cleanup. An owned wineserver spinning alone does not count as build progress. Detached
children are tracked only when their ancestry was observed; unknown processes are never
adopted by name. No automatic prefix-wide reset or retry is performed.

Both DLLs must exist after successful compilation, and OptiScaler must be refreshed when
its stamps changed. Both outputs are prepared before publication; each replacement is atomic
and ordinary promotion errors roll back earlier replacements. The pair is not crash-atomic.
Redirect output to a file. See `tests/build-lifecycle/README.md` for regression coverage and limits.

## test-rendering.sh

Runs every registered host suite in strict mode with four workers, builds Release, then runs
D3D12 camera/texture checks, the moving-scene production NR A/B fixture, offline replay through
the production seam, four NR Vulkan scenarios (`nr`, `nr-route-quiet`, `nr-route-quiet-wdx12`,
`nr-route-guides`), and the complete production DLSS-G provider fixture. Wine/GPU work is serial.
A non-PASS required step stops the sequence and leaves remaining steps as `NOT_RUN`; unavailable
prerequisites cannot become a successful combined run.

```bash
tools/test-rendering.sh                         # complete sequence, without opening a game
tools/test-rendering.sh --host-only             # strict host checks; no GPU coverage
tools/test-rendering.sh --no-build              # use the existing x64/out DLL pair
tools/test-rendering.sh --fg-steady-seconds 15   # three complete FG windows per active phase
tools/test-rendering.sh --report x64/logs/rendering.json --log-dir x64/logs/rendering
```

The shell entry delegates to `test-rendering.py`, which reuses the existing test runner's owned
process supervision. Each invocation gets a unique log directory. The default consolidated report
is `artifacts/rendering/<run-id>/summary.json`; `--report` selects another report path and
`--log-dir` selects the parent for retained invocation logs. The receipt records exact commands,
exit codes, original fixture outcomes, before/after DLL and forwarder hashes, copied fixture
receipts and logs, and raw validation findings. A stale receipt, wrong case set or changed binary
fails the combined check. The optional `--strict` flag is accepted for compatibility; this entry
always requires complete PASS results.

Each GPU suite also retains its own ignored `artifacts/` directory. Models and plugins must
already be available locally. The Vulkan cases cover existing present/route scenarios; they do
not supply an exposure image or request a working scale above 1, so they do not exercise the new
meter's GPU sample path or the supersampling scaler. Focused host tests cover those lifetimes.
Raw Vulkan validation messages remain in the report even when a fixture attributes them outside
its pass criteria. Image metrics and GPU timings remain scenario measurements; the orchestrator
issues no general quality or performance approval. See [the tests index](../tests/README.md).

## deploy-to-game.sh

Copies the built `x64/out/OptiScaler.dll` and forwarder into a game directory under a proxy DLL
name, installs the release support folder and `OptiScaler.ini` only if the game has none, and reports the Vulkan-hook state.
Builds first if there is no output yet. Every change is planned before the first write, and it
writes nothing when any of these holds:

- **The game is running** (exit 3): some process maps a file under the directory, read from
  `/proc/<pid>/maps`. It names the pid, its `comm` and its executable. There is no override. `pgrep -x` is
  not used because `comm` is cut at 15 characters, which is how Cyberpunk was overwritten while it ran
  (`CLAUDE.md`). Files are written to a temp file and renamed into place, so the old inode is never
  truncated even then.
- **A file at one of our names is not ours** (exit 4). Ours means the receipt's hash matches, or the
  file is byte-identical to the build being installed. An OptiScaler build or forwarder with no receipt
  (anything the script deployed before it had receipts) is backed up and replaced without a flag.
  Anything else needs a flag. ReShade at the proxy name takes `--keep-reshade`. Anything else takes
  `--replace-foreign`. A name that exists in another case (`DXGI.DLL`), a symlink or a directory is
  refused too.
- **Another proxy name already holds OptiScaler** (exit 5), e.g. `winmm.dll` when deploying as
  `dxgi.dll`. The Divinity trap: both load and their hooks fight. Override with `--allow-second-proxy`.

`--keep-reshade` renames ReShade (`dxgi.dll` to `ReShade64.dll`, `d3d12.dll` to `d3d12-ReShade64.dll`)
and sets `[Plugins] LoadReshade=true` in `OptiScaler.ini`. It replaces the existing line, keeping its
spelling and line ending, and adds one only when the section has none, because the parser keeps the last
of two. It refuses when `ReShade64.dll` already exists. A user ini it edits is backed up first.

What a deploy wrote, set aside or renamed goes into `<game>/.retoque/receipt.txt`. Backups go into
`<game>/.retoque/backup/<UTC stamp>/`, never `*.bak` beside the DLL. The receipt is tab-separated text
(format in the script's header) and is rewritten atomically after every step. A deploy that stops
part-way leaves it marked `partial`, listing exactly what was done; the next deploy, or `--uninstall`,
picks up from it. `--uninstall` removes only recorded files whose hash still matches, restores the
backups, and undoes the renames (ReShade, libxell). Anything that changed since, such as an ini you
edited, is left in place with a note and stays in the receipt.

```bash
tools/deploy-to-game.sh [options] <game-directory> [dxgi.dll|winmm.dll|version.dll|d3d12.dll|dbghelp.dll|winhttp.dll|wininet.dll|OptiScaler.dll|<name>.asi]
tools/deploy-to-game.sh --uninstall [--dry-run] <game-directory>

  -n, --dry-run           print the plan (and any refusal), write nothing
  --keep-reshade          keep a ReShade proxy as ReShade64.dll, loaded by OptiScaler
  --replace-foreign       replace a file that is not ours; the original goes to .retoque/backup/
  --allow-second-proxy    deploy although another proxy name here holds OptiScaler
  --enable-nr             set [DlssNr] Enabled=true in place, backing up a preserved user ini
  --no-nvngx              skip copying the NVIDIA driver's Wine _nvngx.dll
  --source-dir DIR        take DLLs, ini and OptiScaler/ support files from DIR (never builds then)
```

The ini comes from `--source-dir` when available, falling back to the repository ini. The optional support
folder comes only from the release source: the repository's `OptiScaler/` contains C++ source, not a release.
Unknown support DLLs are refused unless `--replace-foreign` is passed. A target directory link is refused.
Linux uninstall leaves empty support directories in place. `_nvngx.dll` is discovered under
`/usr/lib/nvidia/wine/`, `/usr/lib64/nvidia/wine/` or `/usr/lib/x86_64-linux-gnu/nvidia/wine/`, and copied
with the same receipt and foreign-file rules as the other DLLs. No driver found means a visible warning.
`--enable-nr` and `--keep-reshade` share one backup when both edit a preserved ini.

`tests/deploy-safety` runs it against fake game directories.

## install-linux.sh

Run `bash install-linux.sh` from an extracted release. It asks for the executable folder, proxy name
(default `dxgi.dll`) and whether to turn Neural Rendering on (default yes). With arguments it passes
through to `deploy-to-game.sh`, taking source files from the extracted folder. The shell scripts ship
with LF; invoking bash works even when zip extraction loses the executable bit.

```bash
bash install-linux.sh --enable-nr "/path/to/game/bin64" dxgi.dll
bash install-linux.sh --dry-run --enable-nr "/path/to/game/bin64"
bash install-linux.sh --uninstall "/path/to/game/bin64"
```

## install-windows.ps1 and install-windows.cmd

The Windows counterpart of `deploy-to-game.sh`, for end users: both ship in the release zip beside the DLLs
(`package_release.ps1` refuses to build a release without them). The `.ps1` runs on Windows PowerShell 5.1
and PowerShell 7 with no modules. The `.cmd` is the double-click entry: it asks for the game folder and
whether to install, uninstall or dry-run, and whether to enable NR (default yes), then starts the `.ps1` with `-ExecutionPolicy Bypass` for that one
`powershell.exe` process (no policy is changed for the user or the machine). With arguments it passes them
through and adds nothing.

It installs from its own folder: `OptiScaler.dll` under the proxy name and as `OptiScaler.dll` beside it, the
forwarder, the release's `OptiScaler\` support folder (FSR, XeSS, Agility SDK), and `OptiScaler.ini` only
when the game has none. The refusals, exit codes, Keep-ReShade handling, backups and `-Uninstall` are those of
`deploy-to-game.sh` above. The running-game check is Windows-native: refuse when any process has a module
loaded from the game folder, or when a file it would touch cannot be opened for writing with no sharing (an
elevated or anti-cheat-protected game hides its module list, but not that). The receipt is the same file in
the same format, so either script uninstalls what the other installed; the one difference is that the Linux
script leaves the emptied `OptiScaler\` folders behind. `nvngx_dlssnr.dll`,
NVIDIA's model, is never copied, moved or removed; a missing one is reported.

```powershell
install-windows.cmd                                         # asks
install-windows.cmd -GameDir "D:\Games\Crimson Desert\bin64" [-ProxyName winmm.dll] [-DryRun]
install-windows.cmd -GameDir "D:\Games\Crimson Desert\bin64" -Uninstall [-DryRun]

  -EnableNR          set [DlssNr] Enabled=true with a receipt-recorded ini edit
  -KeepReShade        keep a ReShade proxy as ReShade64.dll, loaded by OptiScaler
  -ReplaceForeign     replace a file that is not ours; the original goes to .retoque\backup\
  -AllowSecondProxy   install although another proxy name here holds OptiScaler
  -SourceDir <dir>    take the files from <dir> instead of the script's folder
```

`tests/windows-installer` runs it against fake game folders in a PowerShell 7 container, crossing receipts
with `deploy-to-game.sh` both ways. Windows itself (a real loaded DLL, NTFS, Windows PowerShell 5.1, the `.cmd`)
is not exercised there; its README says what stands in for what.

## package_release.ps1

Assembles `release/Retoque-vX.Y.Z.zip` (the `retoque-vX.Y.Z` tag without its prefix) from an explicit
allow-list under `release/`; it refuses a stale forwarder, refuses any `Enabled=true` in the packaged ini,
and never ships `nvngx_dlssnr.dll`. It adds
`install-windows.ps1` and `install-windows.cmd` from `tools/` (the `.cmd` rewritten with CRLF line ends) and
refuses when either is missing. It also ships `install-linux.sh`, `deploy-to-game.sh` (LF), the README,
and RenoDX attribution. The release ini sets Home as the menu key and enables file logging; NR remains
off until the installer is asked to enable it. Required missing files and a model hidden in a support
folder are refused. `-SkipBuild` can package fixture build output under PowerShell 7 on Linux as well.

`.github/workflows/retoque-release.yml` builds Release on Windows for `retoque-v*` tags or a manual version
input, packages it and creates **only a draft prerelease**, using `docs/releases/<tag>.md`. The input is
validated and an existing tag must match the selected checkout. Any existing release with that tag
(including a draft) is refused; retries never edit or publish it. If upload fails, inspect the partial
draft manually before retrying. Publication is a separate human action; the workflow never uses `nightly`.

```powershell
pwsh tools\package_release.ps1 [-Version <tag>] [-SkipBuild]
```

## gen-vk-device-chain-types.py

`python3 tools/gen-vk-device-chain-types.py` writes `retoque/hooks/Vulkan_DeviceCreateChainTypes.h`: every
structure `external/vulkan/include/vulkan/vulkan_core.h` defines that `external/vulkan/registry/vk.xml` allows in a
`VkDeviceCreateInfo` chain, with its size. The `vkCreateDevice` hook uses it to copy a game's whole chain into
storage the call owns (`retoque/hooks/Vulkan_OwnedDeviceRequest.h`). Rerun it after `external/vulkan` moves;
`--check` exits 1 while the committed header is stale, and `tests/vk-create-contract` runs that check.

## nrlog_summary.py

Summarises an `OptiScaler.log` from a DLSS-NR run (build, NR coverage, composition changes,
cadence, DLSS-G transitions, errors, GPU timing statistics), or compares two logs side by side.
Plain Python 3, no dependencies.

```bash
python3 tools/nrlog_summary.py <OptiScaler.log> [...]
python3 tools/nrlog_summary.py --compare <LogA> <LogB>
```

## local/

`tools/local/` is gitignored. Keep personal, machine-specific helper scripts there.
