# Retoque v0.3.1

Retoque brings DLSS Neural Rendering (NR), Lossless Scaling frame generation and Proton fixes to games through
OptiScaler, with a desktop app (Windows and Linux) that finds your games and installs, updates and removes Retoque
in each one after showing you exactly what will change.

This release fixes what the first Windows run of v0.3.0 found and lets the number of model passes change while the
game runs. Everything in [v0.3.0](retoque-v0.3.0.md) still applies.

## Assets

| File | What it is |
|---|---|
| `retoque-app-0.3.1-windows-x64.zip` | The Windows app (`retoque-app.exe`) with the Retoque files it installs, licences and translations |
| `retoque-app-0.3.1-linux-x64.tar.gz` | The Linux app and the `retoque` CLI with licences; Retoque's files come from the payload below. Built on Arch Linux: needs glibc 2.43 or newer (older distributions: build from source, `tools/app/README.md`) |
| `retoque-payload-0.3.1-x64.zip` | Retoque itself, as the app installs it: `OptiScaler.dll`, the NR forwarder, `RetoqueWindowHost.exe`, OptiScaler's support folder (XeSS and FidelityFX libraries), release `OptiScaler.ini`, licences, translations |
| `SHA256SUMS` | Checksums of the three files above |
| `Retoque-v0.3.1.zip` | The same Retoque files with the script installers (`install-windows.cmd`, `install-linux.sh`), built by CI |
| `Retoque-v0.3.1.zip.manifest.json` | CI's record of that zip: source commit, member hashes, host-test result |

The DLLs are the ones CI built for `Retoque-v0.3.1.zip`, so every asset carries the same `OptiScaler.dll`. Nothing
from NVIDIA, Lossless Scaling or dlssg_for_sm86 is in these files; `nvngx_dlssnr.dll` (the NR model) is never shipped.

## Start playing

Same as v0.3.0. Windows: extract `retoque-app-0.3.1-windows-x64.zip`, open `retoque-app.exe` and, in Preferences,
choose **Install Retoque on this computer…**. If v0.3.0 is installed, this replaces it in place. Then pick a game and
press **Update** to give it this release's files. Linux: extract the tarball and run `retoque-app`.

## What changed

**Passes change without restarting the game.** On the route after the upscaler (the common one), raising
`Passes` in the in-game menu used to say "Restart required". Now the new passes start within a second. The price is
shown in the menu and the log: the one-pass model that was running stays in video memory until the game closes
(on Dying Light: The Beast, 250 MiB for the model and 64 MiB of surfaces). This happens once per session. After that,
passes go up and down freely and the extra ones are freed when lowered. If the adapter has no room for the second
copy, one pass keeps running and the menu says why. The driver proxy mode and the routes before the upscaler keep
asking for a restart.

**Windows app**
- Uninstalling from Apps & features now removes the program folder as well. In v0.3.0 the cleanup script started
  without a console, could not see the app exit and gave up, leaving `%LOCALAPPDATA%\Programs\Retoque` behind.
- Installing the app over a folder that an earlier uninstall left behind replaces it instead of refusing.
- `RetoqueWindowHost.exe` is in the downloads again, so "NR on any window" can start. The app only offers that
  screen when the host is present beside the DLLs.
- This is a regular release, so the app's update check offers it.

**Also since v0.3.0**
- Synthesized motion for frame generation: FidelityFX's scene-change sums get the barriers they were missing, which
  caused false scene cuts (seen under Proton).
- Character inspection, first slice: a log-only mask inspection behind `[DlssNr] MaskInspection`, off by default.
- `retoque legacy-probe` in the CLI: read-only report of which Retoque route fits a game today.
- GPU test fixtures run again on Linux; frame transport design and its protocol core (not wired into anything yet);
  design notes for 32-bit, D3D10 and OpenGL reach (proposal only).

## Validation

Tagged commit and CI evidence are in `Retoque-v0.3.1.zip.manifest.json`. Before publication (2026-11-20):

- GitHub Actions on the tagged commit `ba355423`: the release workflow re-ran the strict host tier (146 PASS),
  built Release with MSBuild 18.12.2.51402 and wrote the manifest (`dirty=false`, host PASS).
- Locally on the same commit: full host tier (146/146), `check_docs`, app `cargo fmt`, `clippy -D warnings`, the
  app test suite (650 passed), and Windows and Linux app builds.
- Shipped binaries (from the CI build): `OptiScaler.dll` `2a4ab7fee79d46e55f4da75cb355dffbf9196dbc85a00772322a0474e936ba6d`,
  `nvngx.dll_dlssnr.dll` forwarder `ab900f4f2d58501de3f78e8d427ac24e845b65fa4f541af3f28e6afb3a69c687`,
  `RetoqueWindowHost.exe` `a73eecd2519eb2357d0d7cde15c4fe040f7bebea81f5b974aaee74c464a920ad`.
- `SHA256SUMS` matches the three app assets as uploaded.
- Live passes, on native Windows 11 (RTX 3060, driver 621.14), Dying Light: The Beast, model 310.9.16.0, with the
  build of the passes change alone (`7611a175`, `OptiScaler.dll` `09b93915…`; this release adds the merges listed
  above on top). Started at Passes=1 after DLSS, then changed in the menu to 2, 10, 1, 4 and 2 without restarting:
  each value reached `requested=N completed=N`, the switch logged the retained 250 MiB + 64 MiB, GPU time per
  evaluation went from 11.2 ms (1 pass) to 22 ms (2) and 107 ms (10), and back to 40 fps at 1 pass. Then gameplay at
  2 passes, a pause and resume, and a normal exit: 15,747 evaluations, `model_failed=0`, no device removal.
- Wine fixtures on that same build (RTX 4090): all pass; one intermittent synthesized-motion case was the
  scene-cut bug fixed in this release.

**Not validated:** this release's exact DLLs in a game, the budget refusal on a nearly full adapter, a resolution
change after switching, Linux/Proton runs, other games, and, on native Windows, uninstalling 0.3.1, the leftover
replacement and "NR on any window".

## Known issues

- If you already uninstalled v0.3.0, its folder may still be in `%LOCALAPPDATA%\Programs\Retoque`. Installing
  v0.3.1 replaces it; you can also delete it by hand.
- The app still labels the Retoque files bundled in its zip "Local build" instead of 0.3.1; files it downloads from
  this release show the version.
- After a live switch the menu's model memory figure includes the copy kept from the one-pass run, and fewer extra
  passes may fit than after a restart.

Report problems in the issues.

This combines ideas and fixes from OptiScaler, Dagherbou, scottmudge, janblade, wilsjo2, RenoDX and other
contributors with this fork's own work and regression tests. See the README credits and the included licences.
