# Windows desktop app installation and updates

The Windows app offers **Install Retoque on this computer…** in Preferences when it is running outside
`%LOCALAPPDATA%\Programs\Retoque\retoque-app.exe`. The review lists the destination, every copied file, the Start
Menu and Desktop shortcuts and the current-user Apps & features registration. This installs the app itself;
installing Retoque into a game remains a separate reviewed operation. Linux shows none of these controls.

The complete payload must be beside the running executable: `OptiScaler.dll`, `nvngx.dll_dlssnr.dll`,
`OptiScaler.ini`, `Licenses/`, `translations/` and optional `OptiScaler/`. The app copies only the release allowlist,
checks source hashes again after review and verifies its staged copies. It refuses an occupied destination,
existing shortcuts/registration and linked paths. Use the installed copy's update control for an existing install;
a portable copy never overwrites it. Native `IShellLinkW` creates the shortcuts, using Windows known folders
(including a redirected Desktop).

Registration lives at `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\Retoque`, with DisplayName,
DisplayVersion, Publisher, InstallLocation, DisplayIcon, EstimatedSize (KiB), NoModify, NoRepair and an
UninstallString of `"<installed exe>" --uninstall-app`. After successful installation the app offers to restart
from the installed copy. It does not request administrator privileges.

## Uninstall

Apps & features launches `--uninstall-app`. A native confirmation appears before any change. Only the installed
copy accepts this argument. It removes the two shortcuts and registry entry, then a detached hidden `cmd` waits
for that app PID to exit before deleting the program directory. A completion marker prevents directory deletion
if removal of the shortcuts or registration failed. Junctions and symbolic links are refused.

**Neither installation nor uninstallation touches `%LOCALAPPDATA%\Retoque` or Retoque inside games.** Models,
preferences, downloaded components and game receipts remain in their existing locations.

## Release contract and packaging

A release of `gabrielmaialva33/retoque` may additionally attach:

- `retoque-app-<version>-windows-x64.zip`: `retoque-app.exe` plus the [payload contents](PAYLOAD.md).
- `SHA256SUMS`: one line for that archive alongside any payload archive line.

Bump `workspace.package.version` in `tools/app/Cargo.toml` before building a new app release. The executable carries
an embedded version marker; the packager refuses a different release version or an executable that is not x64 PE.
Start with a payload archive produced by the existing script, then package the app:

```sh
bash tools/app/scripts/make-payload-release.sh --build-dir x64/out --out release/app --version retoque-v0.1.0
bash tools/app/scripts/make-app-release.sh \
  --exe tools/app/target/x86_64-pc-windows-gnu/release/retoque-app.exe \
  --payload release/app/retoque-payload-0.1.0-x64.zip \
  --out release/app --version retoque-v0.1.0
```

The second script verifies the payload's internal member checksums, rejects unsafe/forbidden files, adds the app,
regenerates internal checksums and merges the app's checksum into the existing release-level `SHA256SUMS`.
Run it **after** the payload packager, which replaces its own checksums file. Both scripts only print a draft
`gh release create` command. Publish manually and include both archives and the combined checksums file when both
asset types are wanted. GitHub must expose a SHA-256 asset `digest` before the app accepts a download.

## Consent, verification and replacement

Preferences offers **Check for app updates…** only in the installed Windows app. Confirming the dialog contacts
the official repository; no check or download runs at startup. Releases without an app asset are skipped. A newer
version offers **Update the app…**, with a separate download consent and size. Requests use the same bounded HTTPS
host/redirect policy as payload downloads. Both the release `SHA256SUMS` line and the GitHub asset digest must match.
The optional digest of `SHA256SUMS` is also checked when GitHub supplies it.

Verified files are extracted into a unique `.retoque-update-*` directory inside the program folder. The payload's
existing archive validation also applies to the app archive, including the forbidden NVIDIA/LSFG files, link and
collision refusals, size limits, forwarder/OptiScaler checks and INI policy. Before replacement every staged member
is checked again against its manifest; extra files and a mismatched executable version marker are refused.

A second confirmation, **Restart the app**, launches a copy of the current executable as an update helper. It
acquires a file lock, waits for the original process to exit, renames the old app to `retoque-app.old.exe`, retains
old payload roots in the staging directory and moves the verified new roots into place. It updates registration
and relaunches the installed app. Ordinary replacement failures roll back completed renames in reverse order;
registration or launch failure restores the old files. The next installed startup deletes the old executable
only for a committed update, and retries cleanup of committed staging. Downloads cancelled before restart are
removed when their staging owner is dropped. Restart confirmation is disabled while other app jobs are busy.

An unexpected machine shutdown can leave a rename journal and retained old files. Such an incomplete transaction
is **not** silently cleaned on startup: the app refuses to continue. Keep the entire program directory for recovery;
`journal.json` records the rename intents, `retoque-app.old.exe` retains the prior executable and `backup/` retains
moved payload roots. Do not delete those files or treat a partly replaced installation as current. Automatic
power-loss recovery is not implemented. Staging without a journal is an unapplied download and may be removed
while all app instances are closed.

## Validation boundary

Pure host tests cover paths, registry values, reviewed source copies, stale/missing payload refusal, version ordering,
archive restrictions, both digests, staged-byte verification and replacement/rollback ordering. Synthetic packaging
tests run with `python3 tools/app/scripts/test_make_app_release.py`. Windows cross-Clippy checks native API types.
These checks do not execute an installer on Linux and do not qualify Windows shell shortcuts, Apps & features,
process waiting/deletion, antivirus interactions, native GUI behavior or actual restart/update operation. Exercise
install, cancellation, uninstall, update, a locked file and a failed restart on Windows before release.
