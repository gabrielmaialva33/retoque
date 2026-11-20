# Retoque v0.3.2

Retoque brings DLSS Neural Rendering (NR), Lossless Scaling frame generation and Proton fixes to games through
OptiScaler, with a desktop app (Windows and Linux) that finds your games and installs, updates and removes Retoque
in each one after showing you exactly what will change.

A small release with one fix: games with their own DLSS Frame Generation no longer crash at startup when
OptiScaler's frame generation is set up for them. Everything in [v0.3.1](retoque-v0.3.1.md) still applies.

## Assets

| File | What it is |
|---|---|
| `retoque-app-0.3.2-windows-x64.zip` | The Windows app (`retoque-app.exe`) with the Retoque files it installs, licences and translations |
| `retoque-app-0.3.2-linux-x64.tar.gz` | The Linux app and the `retoque` CLI with licences; Retoque's files come from the payload below. Built on Arch Linux: needs glibc 2.43 or newer (older distributions: build from source, `tools/app/README.md`) |
| `retoque-payload-0.3.2-x64.zip` | Retoque itself, as the app installs it: `OptiScaler.dll`, the NR forwarder, `RetoqueWindowHost.exe`, OptiScaler's support folder (XeSS and FidelityFX libraries), release `OptiScaler.ini`, licences, translations |
| `SHA256SUMS` | Checksums of the three files above |
| `Retoque-v0.3.2.zip` | The same Retoque files with the script installers (`install-windows.cmd`, `install-linux.sh`), built by CI |
| `Retoque-v0.3.2.zip.manifest.json` | CI's record of that zip: source commit, member hashes, host-test result |

The DLLs are the ones CI built for `Retoque-v0.3.2.zip`, so every asset carries the same `OptiScaler.dll`. Nothing
from NVIDIA, Lossless Scaling or dlssg_for_sm86 is in these files; `nvngx_dlssnr.dll` (the NR model) is never shipped.

## Start playing

Same as v0.3.1. Windows: extract `retoque-app-0.3.2-windows-x64.zip`, open `retoque-app.exe` and, in Preferences,
choose **Install Retoque on this computer…** (it replaces an installed v0.3.x in place). Then pick a game and press
**Update**. Linux: extract the tarball and run `retoque-app`.

## What changed

**Games with their own DLSS Frame Generation and OptiScaler's frame generation.** With an FG Input such as OptiFG
and an FG Output such as XeFG or FSR FG, OptiScaler makes its own frame generation swapchain. In a game that also
ships Streamline DLSS-G, the game's DLSS-G hooked that swapchain a second time and the game crashed at the first frame
(seen in Forza Horizon 6). Now, on D3D12, the game's DLSS-G hooks are cleared in that case too, as they already were
for FG Input or Output DLSSG, and the game's DLSS-G stays off for the session with its resources freed: OptiScaler's
frame generation replaces it. The log says so, and the menu hides the game's DLSS-G ratio and Dynamic MFG controls,
which would do nothing then. Vulkan games and games on Streamline 1 are unchanged.

With the release `OptiScaler.ini` (FG Input and Output `auto`, which means none) nothing changes: the fix only acts
when you chose an FG Input and an FG Output yourself.

Ported from janblade's [OptiScaler-F5-DLSSNR-Multipass](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass)
PR #347 (GPL-3.0); upstream OptiScaler still clears the hooks only for DLSSG.

## Validation

Tagged commit and CI evidence are in `Retoque-v0.3.2.zip.manifest.json`. Before publication (2026-11-20):

- The fix's branch: full host tier (147, including the new `sl-own-fg-swapchain` suite and an `mfg-provider-capacity`
  case that runs the production `slDLSSGSetOptions` hook and fails without the fix), MSVC Release build, and GitHub
  Actions NR Tests and clang-format on the merge `000c754b`.
- The release commit `51001a8f`: app `cargo fmt`, `clippy -D warnings`, the app test suite (650 passed), the tooling
  suites, Windows and Linux app builds; the CI release workflow re-ran the strict host tier (147 PASS), built Release
  with MSBuild 18.12.2.51402 and wrote the manifest (`dirty=false`, host PASS).
- Shipped binaries (from the CI build): `OptiScaler.dll` `8e45cf336bad9daf9c5d90278655352ce2791af641ce78996eef236bc8f03878`,
  `nvngx.dll_dlssnr.dll` forwarder `0e20230b88f81cd8597918220db0286190746eb54ef73dc0608f6c1ce7b4f7e9`,
  `RetoqueWindowHost.exe` `a2a1d6b1764854000f48f4f171030f7df423f263bd1a49e86c63dc81343c8860`.
- `SHA256SUMS` matches the three app assets as uploaded.

**Not validated:** the fix in a game. Forza Horizon 6 is where janblade saw the crash and its fix on their fork; no
game with its own DLSS-G was run with this build. Everything listed as not validated for v0.3.1 still is.

## Known issues

- The app still labels the Retoque files bundled in its zip "Local build" instead of 0.3.2; files it downloads from
  this release show the version.
- After a live passes switch the menu's model memory figure includes the copy kept from the one-pass run, and fewer
  extra passes may fit than after a restart.
- A folder left by uninstalling v0.3.0 is replaced when you install the app again; you can also delete it by hand.

Report problems in the issues.

This combines ideas and fixes from OptiScaler, Dagherbou, scottmudge, janblade, wilsjo2, RenoDX and other
contributors with this fork's own work and regression tests. See the README credits and the included licences.
