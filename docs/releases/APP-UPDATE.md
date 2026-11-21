# Windows desktop app installation and updates

The Windows release offers `Retoque-Setup-<version>.exe` (below) and the portable
`retoque-app-<version>-windows-x64.zip`. Both end in the same review: the Windows app offers **Install Retoque on
this computer…** in Preferences when it is running outside `%LOCALAPPDATA%\Programs\Retoque\retoque-app.exe`, and
the setup opens it directly. The review lists the destination, every copied file, the Start
Menu and Desktop shortcuts and the current-user Apps & features registration. This installs the app itself;
installing Retoque into a game remains a separate reviewed operation. Linux shows none of these controls.

The complete payload must be beside the running executable: `OptiScaler.dll`, `nvngx.dll_dlssnr.dll`,
`OptiScaler.ini`, `Licenses/`, `translations/`, optional `OptiScaler/` and optional `RetoqueWindowHost.exe`. The app copies only the release allowlist,
checks source hashes again after review and verifies its staged copies. It refuses an occupied destination,
existing shortcuts/registration and linked paths. Use the installed copy's update control or the
[setup](#the-setup-retoque-setup-versionexe) for an existing install; a portable copy never overwrites it. Native `IShellLinkW` creates the shortcuts, using Windows known folders
(including a redirected Desktop).

Registration lives at `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\Retoque`, with DisplayName,
DisplayVersion, Publisher, InstallLocation, DisplayIcon, EstimatedSize (KiB), NoModify, NoRepair and an
UninstallString of `"<installed exe>" --uninstall-app`. After successful installation the app offers to restart
from the installed copy. It does not request administrator privileges.

## The setup (`Retoque-Setup-<version>.exe`)

The setup is the one file a Windows user downloads and double-clicks; the portable archive stays for those who prefer
it. It is the small `retoque-setup` executable (`tools/app/crates/retoque-setup`, no console window, `asInvoker`
manifest so Windows never elevates it) with the release's app archive, `SHA256SUMS` and `release.json` appended,
followed by a fixed trailer: the three lengths, the archive's SHA-256 and the magic `RTQSETUP1\0` (layout in the
crate's `lib.rs`). When run it:

1. reads that bundle from its own file, bounds every length (archive ≤ 256 MiB, `SHA256SUMS` ≤ 64 KiB,
   `release.json` ≤ 64 KiB), checks the archive hash in the trailer and then that `release.json`, `SHA256SUMS` and
   the archive agree, exactly as a downloaded update must (both digests, sizes, official names);
2. creates a fresh private folder `%TEMP%\retoque-setup-*`, writes `bundle.zip`, `SHA256SUMS` and `release.json`
   there and unpacks the archive into `payload\` with the app update's own validator (allowlist, links, collisions,
   size limits, forwarder/OptiScaler checks, INI policy, x64 app). It writes nowhere else;
3. runs `payload\retoque-app.exe --setup <folder>`, waits for it and removes the folder (best effort).

A failure before the app starts shows a native message (English, or Brazilian Portuguese by the Windows language)
with the technical detail, and exits non-zero; so does an app that ends with an error.

`retoque-app.exe --setup <folder>` (exactly these two arguments, an absolute existing folder) opens one review at
startup, chosen by the Apps & features registration:

- **No registration** (a retoque-v0.3.0 uninstall leftover counts as none): the same **Install Retoque on this
  computer** review as Preferences, from the unpacked copy. After installing, the restart offer opens the installed
  copy.
- **An older registered version**: **Update the app** says which version is installed and which the setup carries.
  Confirming stages the setup's own archive inside the program folder with every check of a downloaded update
  (`app_release::stage`), then hands it to the same helper as an ordinary update and closes. The helper is a copy of
  the **installed** executable, not of the running one: `--apply-app-update` refuses a manifest that is not newer than
  the helper's own version, and the setup runs the newer app. Everything after that is the ordinary update below
  (lock, wait for this process, journaled replacement, re-registration, relaunch with `--finish-app-update`).
  A registration without a comparable version is offered as an update; the installed helper still decides.
- **The same or a newer version**: says so and offers **Open Retoque** (the installed copy); nothing changes.

In this mode only **Cancel** ends the dialog, and it closes the app; a failed step is shown in the dialog with its
details and **Try again…**. The setup and the portable archive install the same files into the same place.

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
- `Retoque-Setup-<version>.exe`: the setup above, carrying that archive.
- `SHA256SUMS`: one line per asset: the app archive, the payload archive, the Linux tarball and the setup.

Bump `workspace.package.version` in `tools/app/Cargo.toml` before building a new app release. The executable carries
an embedded version marker; the packager refuses a different release version or an executable that is not x64 PE.
Start with a payload archive produced by the existing script, then package the app:

```sh
bash tools/app/scripts/make-payload-release.sh --build-dir x64/out --out release/app --version retoque-v0.3.0
bash tools/app/scripts/make-app-release.sh \
  --exe tools/app/target/x86_64-pc-windows-gnu/release/retoque-app.exe \
  --payload release/app/retoque-payload-0.3.0-x64.zip \
  --out release/app --version retoque-v0.3.0
```

The second script verifies the payload's internal member checksums, rejects unsafe/forbidden files, adds the app,
regenerates internal checksums and merges the app's checksum into the existing release-level `SHA256SUMS`.
Run it **after** the payload packager, which replaces its own checksums file. Both scripts only print a draft
`gh release create` command. Publish manually and include both archives and the combined checksums file when both
asset types are wanted. GitHub must expose a SHA-256 asset `digest` before the app accepts a download.

To produce the setup in the same step, build the stub and pass it:

```sh
(cd tools/app && cargo build --release --target x86_64-pc-windows-gnu -p retoque-setup)
bash tools/app/scripts/make-app-release.sh ... --setup-stub tools/app/target/x86_64-pc-windows-gnu/release/retoque-setup.exe
```

or run `tools/app/scripts/make_setup.py` on its own (stdlib only):

```sh
python3 tools/app/scripts/make_setup.py --stub <retoque-setup.exe> \
  --app-zip release/app/retoque-app-0.3.3-windows-x64.zip --sums release/app/SHA256SUMS \
  --tag retoque-v0.3.3 [--published-at 2026-12-01T12:00:00Z] --out release/app
# or, instead of --tag/--published-at: --release-json <file>
```

The script checks that the stub is an x64 Windows GUI executable without a bundle, that `SHA256SUMS` lists the
app archive with its real hash, that the app inside is the release version and has the `--setup` mode (an older app
is refused unless `--allow-app-without-setup` is given, which only makes sense to check the format), and that
`release.json` is what the app accepts (`Release::validate_kind(true)`) and names that asset, version, size and
digest. `--tag` builds `release.json` the way the app builds it from the GitHub API (`url`, `sums_url`, `size`,
`digest`, `sums_size`, `sums_digest`); `--published-at` defaults to now and is not compared by the app.

**Which `SHA256SUMS`.** The given `SHA256SUMS` is embedded unchanged, and `release.json`'s `sums_size` and
`sums_digest` describe that embedded copy; it cannot list the setup that contains it. The setup's own line is then
merged into `<out>/SHA256SUMS` (the given file itself when `--out` is its folder, as in the flow above; an existing
line with a different hash is refused). Publish that output file. The final release `SHA256SUMS` must list the app
archive, the payload archive, the Linux tarball and the setup; a line added after the setup was built (the Linux
tarball, say) only changes the published file, never what the setup verifies. After building, check the result
without running it on any platform:

```sh
(cd tools/app && cargo run --release -p retoque-setup --example inspect_setup -- ../../release/app/Retoque-Setup-0.3.3.exe)
```

It reads the bundle as the stub does, unpacks it with the app's validator and stages it as the `--setup` update does.

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
archive restrictions, both digests, staged-byte verification and replacement/rollback ordering, and for the setup the
trailer (round trip, truncation, sizes, magic, hash), extraction confined to its folder, traversal refusal, the
`--setup` argument, the install/update/current decision, the helper's source and staging from the setup's folder.
Synthetic packaging tests (`make_app_release.py`, `make_setup.py`) run with
`python3 tests/app-release-packaging/run.py`. Windows cross-Clippy checks native API types.
These checks do not execute an installer on Linux and do not qualify Windows shell shortcuts, Apps & features,
process waiting/deletion, antivirus interactions, native GUI behavior or actual restart/update operation, nor the
setup itself (its message boxes, its temporary folder, SmartScreen's verdict on an unsigned download). Exercise
install, cancellation, uninstall, update, a locked file and a failed restart on Windows before release, and the
setup's three paths (fresh install, update of an older install, same version) plus a damaged setup file.
