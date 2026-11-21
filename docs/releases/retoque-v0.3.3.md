# Retoque v0.3.3

Retoque brings DLSS Neural Rendering (NR), Lossless Scaling frame generation and Proton fixes to games through
OptiScaler, with a desktop app (Windows and Linux) that finds your games and installs, updates and removes Retoque
in each one after showing you exactly what will change.

This release adds a Windows setup: download `Retoque-Setup-0.3.3.exe` and double-click it. Retoque itself (the DLLs in
your games) is the same as in [v0.3.2](retoque-v0.3.2.md), rebuilt from this commit.

## Assets

| File | What it is |
|---|---|
| `Retoque-Setup-0.3.3.exe` | **Windows: start here.** Installs the app for your user (Start menu, Desktop, Apps & features), or updates an installed v0.3.x |
| `retoque-app-0.3.3-windows-x64.zip` | The same Windows app as a portable archive (`retoque-app.exe` with the Retoque files it installs, licences and translations) |
| `retoque-app-0.3.3-linux-x64.tar.gz` | The Linux app and the `retoque` CLI with licences; Retoque's files come from the payload below. Built on Arch Linux: needs glibc 2.43 or newer (older distributions: build from source, `tools/app/README.md`) |
| `retoque-payload-0.3.3-x64.zip` | Retoque itself, as the app installs it: `OptiScaler.dll`, the NR forwarder, `RetoqueWindowHost.exe`, OptiScaler's support folder (XeSS and FidelityFX libraries), release `OptiScaler.ini`, licences, translations |
| `SHA256SUMS` | Checksums of the four files above |
| `Retoque-v0.3.3.zip` | The same Retoque files with the script installers (`install-windows.cmd`, `install-linux.sh`), built by CI |
| `Retoque-v0.3.3.zip.manifest.json` | CI's record of that zip: source commit, member hashes, host-test result |

The DLLs are the ones CI built for `Retoque-v0.3.3.zip`, so every asset carries the same `OptiScaler.dll`. Nothing
from NVIDIA, Lossless Scaling or dlssg_for_sm86 is in these files; `nvngx_dlssnr.dll` (the NR model) is never shipped.

## Start playing

**Windows.** Download `Retoque-Setup-0.3.3.exe` and run it. It opens the app on one review:

- nothing installed yet: **Install Retoque on this computer** (no administrator rights; it goes to
  `%LOCALAPPDATA%\Programs\Retoque`), then it offers to open the installed app;
- an older v0.3.x installed: **Update the app**, which hands the update to your installed app's own updater (the
  same one its update button uses), then reopens it;
- this version or a newer one already installed: it says so and offers **Open Retoque**.

Then pick a game and press **Update**. The setup is not signed: the first time, Windows SmartScreen may say
"Windows protected your PC"; choose **More info**, then **Run anyway**. The portable zip still works as before.

**Linux.** Extract the tarball and run `retoque-app`.

## What changed

- **Windows setup** (`Retoque-Setup-<version>.exe`): a small executable that carries the app archive, its
  `SHA256SUMS` and the release metadata. It checks them exactly as an update downloaded by the app is checked,
  unpacks them into a private temporary folder, opens the app in setup mode and removes the folder when the app
  closes. Installing, updating and uninstalling are the app's existing code: there is still only one way the app
  installs itself, and the setup is just how it starts.
- The release tooling builds the setup with the app archive and adds it to `SHA256SUMS`.

Retoque in games is unchanged from v0.3.2, including its fix for games with their own DLSS Frame Generation.

## Validation

Tagged commit and CI evidence are in `Retoque-v0.3.3.zip.manifest.json`. Before publication (2026-11-20):

- The setup branch: `retoque-setup` unit tests (bundle trailer, bounds, hash and metadata agreement, unpacking), the
  app's setup decisions (install, update, current) and helper choice, the new `app-release-packaging` host suite,
  app `clippy -D warnings` on Linux and for Windows, and a setup built from the published v0.3.2 assets and read back
  with the setup's own code.
- The release commit `285ccc6f`: app `cargo fmt`, `clippy -D warnings` (Linux and Windows), the app test suite (660
  passed), the tooling suites, Windows and Linux app builds; the CI release workflow re-ran the strict host tier
  (148 PASS), built Release with MSBuild 18.12.2.51402 and wrote the manifest (`dirty=false`, host PASS).
- Shipped binaries (from the CI build): `OptiScaler.dll` `2c8fccd7e9f2a5038f93262e449ddebe51d6ec9bec93aa2ce73d58bcdd2697dc`,
  `nvngx.dll_dlssnr.dll` forwarder `a3c9bee2078c2fe351e9eff17ba1b9e4cf55837e301d16c3c27cbb3317301936`,
  `RetoqueWindowHost.exe` `957e298a92213901b252324e96572d2e50aa5ae13cb110dd84a9c0413f1ff961`.
- `Retoque-Setup-0.3.3.exe` read back with the setup's own code: the bundle checks, the app's validator unpacks its 41
  files and stages them as an update to 0.3.3. `SHA256SUMS` matches the four app assets as uploaded.
- Packaging fix found while building this release: `make_setup.py` looked for the text `--setup` in the app to tell a
  setup-capable app from an older one, but an optimized build never stores that short argument whole; it now uses
  the app's version marker (0.3.3 and newer have the setup mode).

**Not validated:** the setup on Windows. It has not run on a Windows PC yet: the install and update reviews in setup
mode, the hand-off to an installed v0.3.0's updater, the temporary folder's cleanup and how SmartScreen and antivirus
treat it are untested. If the setup fails, the portable zip and the app's own update button work as in v0.3.2.
Everything listed as not validated for v0.3.2 still is.

## Known issues

- The setup and the app are not signed (SmartScreen warning on first run, see above).
- The app still labels the Retoque files bundled with it "Local build" instead of 0.3.3; files it downloads from
  this release show the version.
- After a live passes switch the menu's model memory figure includes the copy kept from the one-pass run, and fewer
  extra passes may fit than after a restart.

Report problems in the issues.

This combines ideas and fixes from OptiScaler, Dagherbou, scottmudge, janblade, wilsjo2, RenoDX and other
contributors with this fork's own work and regression tests. See the README credits and the included licences.
