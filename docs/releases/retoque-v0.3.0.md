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
depth observer, and "NR on any window" on Windows (built and host-tested, not run on Windows) are present but are not
something to rely on yet.

## Limits to keep in mind

Experimental and unsupported by NVIDIA. The model is user-supplied or downloaded after your confirmation, and
hardware compatibility depends on its build (the official 310.9.16 model runs only on RTX 50). No universal FPS or
image-quality improvement is promised. Games outside the profile list may still need route keys in the ini.

## Validation

To be completed before publication with exact hashes: CI host tier on the tagged commit, Release build, Windows app
install/uninstall and a game install on native Windows 11 (RTX 3060), game smoke tests on Linux/Proton (RTX 4090).

This combines ideas and fixes from OptiScaler, Dagherbou, scottmudge, janblade, wilsjo2, RenoDX and other
contributors with this fork's own work and regression tests. See the README credits and the included licences.
