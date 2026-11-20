# Retoque v0.3.0 (pre-release)

Retoque brings DLSS Neural Rendering (NR), Lossless Scaling frame generation and Proton fixes to games through
OptiScaler, with a desktop app (Windows and Linux) that finds your games and installs, updates and removes Retoque
in each one after showing you exactly what will change.

This is Retoque's first published release. The earlier `retoque-v0.1.0` draft and an October `v0.3.0` draft were
never published; their notes are kept in git history.

**Pre-release. Not qualified on the full game route matrix, MFG x3/x4 or every native Windows path yet**
(see "Validation" below).

## Assets

| File | What it is |
|---|---|
| `retoque-app-0.3.0-windows-x64.zip` | The Windows app (`retoque-app.exe`) with the Retoque files it installs, licences and translations |
| `retoque-app-0.3.0-linux-x64.tar.gz` | The Linux app and the `retoque` CLI with licences; Retoque's files come from the payload below. Built on Arch Linux: needs glibc 2.43 or newer (older distributions: build from source, `tools/app/README.md`) |
| `retoque-payload-0.3.0-x64.zip` | Retoque itself, as the app installs it: `OptiScaler.dll`, the NR forwarder, OptiScaler's support folder (XeSS and FidelityFX libraries), release `OptiScaler.ini`, licences, translations |
| `SHA256SUMS` | Checksums of the three files above |
| `Retoque-v0.3.0.zip` | The same Retoque files with the script installers (`install-windows.cmd`, `install-linux.sh`), built by CI |
| `Retoque-v0.3.0.zip.manifest.json` | CI's record of that zip: source commit, member hashes, host-test result |

The DLLs are the ones CI built for `Retoque-v0.3.0.zip`, so every asset carries the same `OptiScaler.dll`. Microsoft's
DXC compiler (for the LSFG shaders) is offered by the app as a verified download when first needed. Nothing from
NVIDIA, Lossless Scaling or dlssg_for_sm86 is in these files. `nvngx_dlssnr.dll` (the NR model) is
never shipped: the app offers a compatible one from its source after you confirm, or you supply your own.

## Start playing

**With the app (recommended).** Windows: extract `retoque-app-0.3.0-windows-x64.zip` to a folder, open `retoque-app.exe`
and, in Preferences, choose **Install Retoque on this computer…** to get Start menu and Desktop
shortcuts and an entry in Apps & features. Pick a game, press the main button and confirm the review. Linux: extract
the tarball and run `retoque-app`; it downloads Retoque's files from this release after you confirm.

**With the scripts.** Extract `Retoque-v0.3.0.zip`, close the game, run `install-windows.cmd` (Windows) or
`bash install-linux.sh` (Linux), select the executable folder and proxy name and accept **Turn Neural Rendering on?**
Under Proton, set `WINEDLLOVERRIDES="dxgi=n,b" %command%` for the default proxy (or your proxy's name).

The app, the CLI and both scripts share one receipt format, so any of them updates or removes what another installed.
Home opens the in-game menu.

## What is in it

**Desktop app**
- Finds games on Steam, Heroic, Lutris, Epic, GOG, Xbox and your own folders, with covers; play and favourites.
- Every change is a reviewed plan with a receipt and backups; refuses while the game runs; refuses 32-bit games;
  asks for an explicit acknowledgment on games with anti-cheat.
- Gets what each game needs from its official source after you confirm, SHA-256 checked: NVIDIA DLSS 310.9.16,
  dlssg_for_sm86 for RTX 20/30, Microsoft DXC, and Retoque updates from this repository's releases.
- DLSS NR model from the community catalog (not an NVIDIA source), with a warning when it has no kernels for your GPU.
- Prepares the Lossless Scaling shaders from your own `lsfg-vk.dll`, never copying or redistributing them.
- One-button "Optimize automatically", doctor with reviewed repairs, log diagnosis, capture and comparison, measure
  this game, Ctrl+K palette, light and dark themes.
- Windows: installs itself for the current user and checks for app updates only when you ask.
- English, Brazilian Portuguese and eight more languages, with an editor for language corrections.

**Neural Rendering**
- NR after the upscaler on D3D12 and Vulkan, present-time paths for games without an upscaler, and a D3D11-to-D3D12
  bridge; game profiles for titles that need a particular route.
- Up to ten model passes, changeable live on the present route; working scale, cadence, exposure and colour
  composition (based on RenoDX), comparison views and GPU timing.
- In-game cost advisor that measures what NR costs on your GPU and can undo what it tried; a simpler panel with the
  full settings one click away.
- NR hand-over to neural ReShade add-ons and AMDLSS5 when one is present.
- RTX 3060 fixes from real play: architecture detection, locale-safe numbers, present pacing, comparison capture on
  textures created with tight alignment, guide copies and NR scope.

**OptiScaler and Proton**
- OptiScaler upstream `optiscaler/master` up to `4a8d55a9`, including the OptiSharpen sharpener.
- Proton: Vulkan overlay and swapchain guards, DLSS-G fixes, NR ordered ahead of the game's frame generation, and the
  RTX 30 frame-generation x3 proposal.

**Not active in this release.** The Vulkan multi-pass chain (admission and contracts only, off), the log-only present
depth observer, and "NR on any window" on Windows are present but are not something to rely on yet. "NR on any
window" also cannot start from these downloads: `RetoqueWindowHost.exe` was left out of them, so the screen reports an
incomplete payload (see Known issues).

## Limits to keep in mind

Experimental and unsupported by NVIDIA. The model is user-supplied or downloaded after your confirmation, and
hardware compatibility depends on its build (the official 310.9.16 model runs only on RTX 50). No universal FPS or
image-quality improvement is promised. Games outside the profile list may still need route keys in the ini.

## Validation

Tagged commit `d3bd1ea6`, clean tree. What was checked before publication (2026-11-20):

- GitHub Actions on the tagged commit: NR Tests (strict host tier) and clang-format pass; the release workflow
  re-ran the host tier, built Release with MSBuild 18.11.1.51303 and wrote the manifest (`dirty=false`, host PASS).
- Locally: host tier 144/144, `check_docs` PASS, app `cargo fmt`, `clippy -D warnings`, 604 tests, Windows and
  Linux app builds.
- Shipped DLLs (from the CI build): `OptiScaler.dll` `9460a52b12fe75c6873ca70afd18182d6cc3f2b77a3fb00aad5451ea8e7986f3`,
  `nvngx.dll_dlssnr.dll` forwarder `05f6bd8073c6eafb3a4754a0fc739d0424e4f5e8babaa899530a5c188829c8f6`.
- `SHA256SUMS` matches the digests GitHub reports for the three app assets.

After publication, on native Windows 11 (RTX 3060, driver 621.14), with the published
`retoque-app-0.3.0-windows-x64.zip` (hash checked on the machine):

- The app opened from the extracted zip, found the GPU and the games, and installed itself from Preferences:
  program folder, Start menu and Desktop shortcuts, and the Apps & features entry at version 0.3.0. It restarted into
  the installed copy.
- "Update and optimize" on Dying Light: The Beast replaced the proxy, `OptiScaler.dll` and the forwarder with the
  hashes above and added the OptiScaler support files. The game, started from the app, loaded this build (menu header
  `d3bd1ea6`). NR ran on DLSS output at Passes=1, 960x540 render to 1920x1080, model 310.9.16.0: 7,332 applied
  compositions, 0 model failures, about 11.1 ms of confirmed GPU time per evaluation, with frame generation x4 at
  times. A few minutes only; image quality was not judged.
- Uninstalling the previous app (0.1.0) from its Apps & features command removed the entry and the shortcuts but left
  the program folder (see Known issues).

**Not validated:** uninstalling 0.3.0 itself, other games, Linux/Proton runs of this build, and longer play.

## Known issues

- **Uninstall leaves the program folder behind** (Windows). The cleanup script is started without a console, where
  `tasklist` prints nothing, so it gives up before removing `%LOCALAPPDATA%\Programs\Retoque`. The Apps & features
  entry and the shortcuts are removed. Delete the folder by hand after uninstalling. Fixed in the next release.
- **"NR on any window" cannot start**: `RetoqueWindowHost.exe` is missing from the downloads.
- The app labels the bundled Retoque files "Local build" instead of 0.3.0, and its update checks skip pre-releases,
  so it will not offer this version as an update. Download it from this page.

Report problems in the issues.

This combines ideas and fixes from OptiScaler, Dagherbou, scottmudge, janblade, wilsjo2, RenoDX and other
contributors with this fork's own work and regression tests. See the README credits and the included licences.
