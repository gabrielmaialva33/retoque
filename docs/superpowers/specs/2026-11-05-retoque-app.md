# Retoque app: one installer and manager for Linux and Windows

Status: **design, 2026-11-05.** Nothing here is implemented yet; it is the contract the first implementation
follows. Decisions taken with the user on 2026-11-05: Rust with egui, one install engine shared by the app and a
command line, and four areas in the first version (library and install, per-game settings, NR model per game, logs
and diagnostics).

## Why

Today a user installs Retoque with one of two scripts that implement the same rules twice:
`tools/deploy-to-game.sh` (bash, Linux/Proton) and `tools/install-windows.ps1` (PowerShell). They agree on the
receipt format, and `tests/deploy-safety` and `tests/windows-installer` hold them to it, but every new rule has to be
written, reviewed and tested twice. Settings are changed by editing `OptiScaler.ini` by hand or in the in-game menu,
the NR model is whatever `nvngx_dlssnr.dll` sits beside the game, and logs are read with `tools/nrlog_summary.py`.
NeuRotic ships a Windows-only Hub (ImGui on Win32/D3D11, C# discovery); it is a reference for features, not code to
port.

## Shape

A Cargo workspace in `tools/app/`, one executable per platform, no runtime dependency beyond the OS:

| Crate | Kind | Owns |
|---|---|---|
| `retoque-core` | lib | receipt v1, install/uninstall plan and apply, the refusals, `OptiScaler.ini` in-place editing, PE version reading, Proton path mapping |
| `retoque-discovery` | lib | finding games: Steam, Heroic, Lutris, manual folders; Epic and GOG on Windows; which folder holds the executable |
| `retoque-data` | lib | NR model catalog, settings schema, log summary, diagnostic bundle |
| `retoque-cli` | bin `retoque` | `install`, `uninstall`, `status`, `--dry-run`; the scripts' replacement |
| `retoque-app` | bin `retoque-app` | the egui interface on top of the three libraries |

Linux builds natively; the Windows build is cross-compiled from Linux with `x86_64-pc-windows-gnu` (mingw). The
libraries do no I/O on the UI thread's behalf beyond what the call names; the app runs plan/apply on a worker
thread and shows progress events.

## Install engine (retoque-core)

It is `tools/deploy-to-game.sh` and `tools/install-windows.ps1` as one implementation. Those two files and their
test READMEs are the specification; where they differ, the difference is reported, not resolved silently.

- **Receipt v1**, byte-compatible: `<game>/.retoque/receipt.txt`, tab-separated, `version`, `status`
  (`complete`, `partial`, `uninstall-incomplete`), `updated`, `proxy`, `file`, `backup`, `rename`; paths relative
  with `/`; rewritten through a temp file and rename after every step. A receipt written by either script is read
  and undone by the engine, and the reverse.
- **Plan before write.** `plan_install` and `plan_uninstall` compute every step and every refusal with no write;
  `--dry-run` and the app's confirmation screen print the plan. `apply` carries it out one step at a time.
- **Refusals and exit codes** as the scripts: 3 a process maps or holds a file in the game folder (Linux reads
  `/proc/<pid>/maps`; Windows enumerates process modules and tries exclusive opens), 4 a file at one of our names is
  not ours, 5 another proxy name already holds OptiScaler; 1 usage or error. Ours is decided by receipt hash or the
  marker strings (`OptiScaler working as`, `dlssnr_call_`; `ReShade` only after OptiScaler is ruled out).
- **Ini rule:** a key is replaced in place, never added beside an existing one (the parser keeps the last); added
  once in its section when missing; CRLF and byte content elsewhere preserved. The same editor serves settings.
- **Never touched:** `nvngx_dlssnr.dll` in a game folder is never copied, moved or removed by install/uninstall.
  Linux copies the driver's Wine `_nvngx.dll` as the script does, with `--no-nvngx`.
- **Proton launch option.** Proxies that Wine also provides need `WINEDLLOVERRIDES="<name>=n,b" %command%`. The
  first version shows it with a copy button; it does not edit Steam's `localconfig.vdf`.
- The payload is found the way the scripts find it: beside the executable in a release, `x64/out` in a checkout,
  or an explicit `--source-dir`.

Parity is proved by running the existing `tests/deploy-safety` cases against `retoque` as well as the bash script,
and the cross-receipt cases of `tests/windows-installer`. The scripts are retired only after that passes, in a
separate change.

## Discovery (retoque-discovery)

Linux: Steam (native `~/.local/share/Steam`, `~/.steam/root`, Flatpak `~/.var/app/com.valvesoftware.Steam`;
`libraryfolders.vdf`, `appmanifest_*.acf`, `compatdata/<appid>` for Proton), Heroic (Legendary, GOG and Nile
installed lists), Lutris (`pga.db`, game YAML). Windows: Steam (registry `SteamPath`), Epic (`.item` manifests), GOG
(registry). Manual folders on both. Each result names its store, install folder, runtime (Windows native, Proton
with app id and prefix, Wine prefix), and candidate target folders, the folder that holds the game's executable
(Unreal: `Binaries/Win64`), ranked, with hints (DLSS DLLs present, an existing receipt or OptiScaler). Parsers are
pure functions over file contents with fixtures; nothing is launched and no store API is called.

## Data (retoque-data)

- **Model catalog.** The user imports their own `nvngx_dlssnr.dll` files; each is copied into the app's data folder
  (`~/.local/share/retoque/models/<sha256>/`, `%LOCALAPPDATA%\Retoque\models\<sha256>\`) and listed with sha256,
  file version and a label. Choosing one for a game sets `[DlssNr] ModelFile` to its absolute path (Windows form,
  `Z:\…`, under Proton/Wine) through the ini editor; `auto` restores the default search
  ([model-file](../../dlssnr/design/model-file.md): a named file that is missing makes NR unavailable, no
  fallback). The catalog never downloads or ships a model, and Git never holds one.
- **Settings schema.** A curated list of `OptiScaler.ini` keys the app edits (NR on/off, Stage, WorkingScale,
  ModelFile, upscaler and frame generation choices), each with section, type, allowed values and a short text. A
  test checks every key exists in the shipped `OptiScaler.ini`.
- **Logs.** Finds a game's `OptiScaler.log` and summarizes it: DLSS version, NR initialization and route, errors,
  coverage and GPU timing, following `tools/nrlog_summary.py`.
- **Diagnostic bundle.** A zip with the log, the ini, the receipt and the status, home paths and user names
  replaced, for the user to send by hand.

## Interface (retoque-app)

A library on the left (search, store filter, installed state), the selected game on the right in four tabs:
Install (state from the receipt, plan preview, install/update/uninstall, launch option), Settings, Model, Logs. Every
write shows its plan first. Languages: English and pt-BR. No control for a feature that is unavailable for that
game.

## Not in the first version

Editing Steam launch options, downloading Retoque releases or models, Windows elevation, signing, and installing
into a running game in any form.

## Validation

`cargo test` for each library on fixtures; the parity suites above; the Windows binary built and its CLI run
read-only (`status`, `--dry-run`) on a Windows machine only with the user's authorization; the app checked by eye on
this workstation. None of this proves a game works with Retoque; that stays per game in
[capabilities](../../capabilities.md).
