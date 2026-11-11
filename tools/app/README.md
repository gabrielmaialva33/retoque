# Retoque app

One installer and manager for Linux and Windows: find games, install, update and remove Retoque, change its
settings per game, choose the NR model per game and read the logs. Design and scope:
[2026-11-05 spec](../../docs/superpowers/specs/2026-11-05-retoque-app.md).
The window's building blocks, rules and glossary are in [crates/retoque-app/DESIGN.md](crates/retoque-app/DESIGN.md).

| Crate | What it is |
|---|---|
| `retoque-core` | The install engine: receipt v1, plan and apply, the refusals, `OptiScaler.ini` in-place editing |
| `retoque-discovery` | Steam, Heroic, Lutris, Epic, GOG and manual folders; which folder holds the executable |
| `retoque-data` | NR model catalog, the settings the app edits, log summary, diagnostic bundle |
| `retoque-cli` | `retoque install / uninstall / status`, the scripts' command line |
| `retoque-app` | The egui window on top of the three libraries |

## Build and run

```bash
cd tools/app
cargo run -p retoque-app                       # the window, from a checkout (uses x64/out as the payload)
cargo run -p retoque-cli -- status "<game dir>"
cargo build --release --target x86_64-pc-windows-gnu -p retoque-app -p retoque-cli   # Windows binaries
cargo test --workspace
```

The Windows build needs `rustup target add x86_64-pc-windows-gnu` and mingw-w64. Run cargo inside a memory-capped
scope on the workstation (see the repository's CLAUDE.md).
Run Cargo from `tools/app` so `.cargo/config.toml` statically links the MinGW C++ runtime. If invoking Cargo with
`--manifest-path` from another directory, also set `CXXSTDLIB_STATIC_x86_64_pc_windows_gnu=1`; the kit script does so.

The payload, the files that go into a game folder, is found beside the executable in a release, under `x64/out` when
the app runs from a checkout, in `RETOQUE_SOURCE_DIR`, in the folder chosen in the window, or downloaded after
confirmation from our official GitHub releases (`gabrielmaialva33/retoque`).

## Rules the app keeps

- Every write is a plan first: the window shows it, and nothing is written while the plan has a refusal.
- A running game is never written to: install, uninstall and settings all refuse while a process holds a file in
  the game folder.
- The receipt is the one tools/deploy-to-game.sh and tools/install-windows.ps1 write; each undoes the other's.
- `nvngx_dlssnr.dll` is never copied into, moved in or removed from a game folder. Models imported into the catalog
  are the user's own files, kept in the app's data folder, chosen with `[DlssNr] ModelFile`.
- Settings are written key by key into the ini on disk, so a change the in-game menu made to another key survives.

## Languages

The window speaks English and Brazilian Portuguese, plus German, Spanish, French, Japanese, Korean, Polish, Russian
and Simplified Chinese. Preferences (and the top bar) list each language by its own name; a choice applies at once
and is saved. On first run the app follows the system locale when it names one of these languages (any Portuguese
locale gives pt-BR; Traditional Chinese does not match), else English. `RETOQUE_LANG=de` (any of the codes below)
forces one for tests and screenshots.

| Code | Language | Source |
|---|---|---|
| `en`, `pt-BR` | English, Português (Brasil) | The `(key, en, pt)` table in `crates/retoque-app/src/i18n.rs`, plus `retoque-data`'s messages and the settings schema |
| `de`, `es`, `fr`, `ja`, `ko`, `pl`, `ru`, `zh-CN` | Deutsch, Español, Français, 日本語, 한국어, Polski, Русский, 简体中文 | `crates/retoque-app/src/i18n/<code>.tsv` |

The eight packs are **machine-translated drafts pending review by native speakers** (each file's header says so,
and Preferences tells the user). A pack is one `key<TAB>text` per line, in the English table's order, `#` starting a
comment; it is embedded at build time and parsed the first time the language is used. A key missing from a pack
shows in English, so a new English string never needs eight translations to ship. Besides the table's keys a pack
translates per-game settings (`setting.<Section>.<Key>` and `.help`), their drop-down values (`choice.<english
label in snake case>`) and, for languages with more plural forms (Polish, Russian), `_few` / `_many` beside a
counted word's `_one`. Tests reject an unknown or duplicate key, a lost or added `{placeholder}`, a final "…" that
does not match the English (DESIGN.md: "…" means a dialog opens) and a missing draft header; they also draw every
pack string with egui's fonts (Latin and Cyrillic) or the system CJK font, failing on a missing glyph, and draw the
main views in every language in a 1024×768 window. To see what a pack still shows in English:
`cargo test -p retoque-app untranslated_keys -- --ignored --nocapture`.

To add a language: add a `Lang` variant (never rename one; the saved choice is the variant name) with its native
name and code, a `pack_source` arm with `include_str!("i18n/<code>.tsv")`, its decimal separator, date order and
plural rule (`plural_category`), and the pack itself, starting from DESIGN.md's glossary so that each idea keeps
one word. Corrections to an existing pack only touch its `.tsv`.

Chinese, Japanese and Korean need a font egui does not ship and the app does not bundle. A font provider
(`src/fonts.rs`) takes one from the system the first time such a character is drawn: on Windows Yu Gothic, Meiryo,
Malgun Gothic, Microsoft YaHei or SimSun from `%WINDIR%\Fonts`; on Linux Noto Sans CJK (Arch, Debian/Ubuntu and
Fedora paths), IPA Gothic, Nanum Gothic or WenQuanYi Micro Hei, and when none of those paths exists, one bounded
`fc-match` query per script. Han characters use the face of the reader's region. The file is read once per process,
and only when needed. Without such a font the selector lists that language disabled, by its name in the current
language, with the reason, and a saved CJK choice opens in English rather than in boxes.

## What is not proven here

The unit tests and the parity suite run on fake game folders. They do not prove that a game works with Retoque, that
the Windows build behaves on a real Windows machine, or anything about image quality; see
[capabilities](../../docs/capabilities.md).

## Static NR model and version checks

The model picker and autotune compare the detected CUDA compute capability with an exact SHA256/size table of
locally inspected model files. Only hashes, sizes and CUDA target lists ship. A matching target is a prerequisite,
not runtime qualification. Unknown hashes are scanned for CUDA ELF ABI 7/8 and fatbin v1 target headers, including
compressed entries; their compatibility remains explicitly unknown because a union of targets does not prove
complete kernel coverage or rule out a PTX fallback. Results are cached by actual content hash, never filename.
The catalog metadata format remains schema 1. Reads are limited to 512 MiB per pass and bounded header probes.

Game status/diagnostics read the PE VERSIONINFO resource of the recognized OptiScaler proxy and NR forwarder.
Missing, malformed or conflicting resources are unknown; two known unequal file versions produce a warning,
without changing install eligibility. These checks never load a DLL or execute GPU code.

## Read-only doctor and evidence APIs

`retoque doctor [--json] <game-dir>` reads installation status, settings, the selected model, local GPU identity,
LSFG readiness and the newest recognized log. Exit **0** means no findings in the available evidence; **6** means
findings were reported; **1** means usage or inspection failed. The command never repairs or creates a catalog.
Text output supports `RETOQUE_LANG=en` and `RETOQUE_LANG=pt`; JSON carries stable IDs, localized-message keys,
evidence and repair proposals. No findings is not runtime qualification.

`retoque_data::doctor::diagnose(&Inputs)` is pure. `Repair` only describes settings, reinstall/reconciliation or
component removal; a caller must build a fresh core `Plan`, display it and honor all refusals before applying it.
Supply the selected game's runtime, actual launch-configuration evidence, model inspection, catalog and GPU.
Unknown is not absent: an unresolved Wine drive cannot prove a model missing, and an absent log cannot prove a
missing launch override. The CLI deliberately leaves runtime/launch options and catalog alternatives unknown
because a folder alone does not identify the launch configuration. Multiple detected GPUs leave GPU choice unknown.
The API can propose an exact compatible catalog model when the caller supplies one.

`logs::diagnose` uses production NR messages and returns redacted log evidence. It keeps initialization,
CPU-observed evaluation and confirmed GPU timing distinct. A driver probe alone is not proof that `_nvngx.dll`
is missing; that diagnosis also requires Wine/Proton context and an explicit absent-file inspection.
Paths with recognized home/userdata components are redacted; this is not a general secret scanner.

Recommendations include facts, confidence and an optional qualification message (`caveat_key`).
The sm86 0.4.1 README specifies Windows 10/11 x64 and D3D12; its Vulkan note points to community patches and limited
coverage. Therefore Proton/Wine suggestions are experimental, and a last-session D3D11/Vulkan API suppresses the
sm86 proposal. The verified confidence tier is reserved for feature-specific runtime proof: API detection or NR
initialization never upgrades a frame-generation proposal. UI consumers can translate the new keys through
`retoque_data::messages::text`; existing recommendation title/why keys remain unchanged.

## Automatic Lossless Scaling shader preparation

The LSFG card finds the user's `lsfg-vk.dll` and prepares all 99 shaders on the heavy worker lane. It uses a bounded
PE resource reader, statically linked SPIRV-Cross (`spirv-cross2` 0.7.2), and dynamically loaded Microsoft DXC
v1.8.2505.1. There is no Python, Wine, command-line compiler, or system DXC dependency. SM 6.0, `main`, `-O3`, and
the binding-to-register-space remap match `tools/lsfg-study/convert.py`; this does not enable native FP16.

DXC libraries are looked up beside the app executable, then in the app data folder under `dxc/v1.8.2505.1`.
If missing, the UI asks before downloading the pinned official Microsoft GitHub release over HTTPS. The only
redirect destination permitted is GitHub's release asset CDN (`release-assets.githubusercontent.com`). The download
has a 64 MiB cap, a timeout and a compiled-in SHA-256; archive extraction only accepts the needed library and
license filenames. The app performs no other network operation. Tests use injected fetchers and never download.

App data is `$XDG_DATA_HOME/retoque` or `$HOME/.local/share/retoque` on Linux, `%LOCALAPPDATA%/Retoque` on Windows.
Completed shaders live in `lsfg-cache/<sha256-of-user-dll>/0.cso` through `98.cso`. Publication uses a temporary
sibling directory followed by rename; a manifest verifies the pipeline version and each shader's SHA-256 before
reuse. Cancel is checked between shaders and download chunks; an active DXC call completes before cancellation.
The source folder and all game folders are untouched during preparation. Enabling LSFG proposes `[LSFG] ShaderDir`
to that cache through the existing settings review, with `Z:\...` mapping under Wine/Proton. Cache readiness is
not GPU, image-quality or Windows runtime qualification.

The library entry point is `retoque_data::lsfg::ensure_lsfg_shaders` (or `lsfg_prepare::ensure_lsfg_shaders`), with
explicit source, data and executable directories, download consent, a cancellation flag and progress callback.
`lsfg_prepare::prepared_cache` verifies the app cache; `lsfg::lsfg_preparation_readiness` returns `Ready` for it. Existing `lsfg_readiness_for_game` continues to inspect the
actual game configuration, so a prepared but unconfigured app cache does not hide a game's invalid ShaderDir.
`lsfg_prepare::shader_dir_edit` returns the settings proposal for callers such as one-click optimization.

Windows kit builds run `bash tools/app/scripts/fetch-dxc.sh <kit-directory>` to download and verify the pinned
Windows package, stage `dxcompiler.dll` and `dxil.dll` beside the app, and include its license files. This is a
build-time download only, not a game payload. The workstation's `build-kit.sh` invokes this helper. Never put
Lossless Scaling DLLs, SPIR-V or generated `.cso` files into a distribution or Git.

Offline parity/diagnostic runner (all paths are supplied explicitly):

```bash
cargo run -p retoque-data --example prepare_lsfg -- '<Lossless Scaling folder>' '<app data folder>' '<DXC library folder>'
```

## Official frame-generation cache and automatic review

The frame-generation card gets `sdli1995/dlssg_for_sm86` from its official GitHub release after confirmation.
A single detected RTX 20/30 gets this offer on first run. The download runs on the worker lane (synchronous
ureq/rustls), follows only approved HTTPS GitHub asset redirects, and is capped at 256 MiB compressed,
1 GiB expanded and 8192 entries. GitHub's SHA-256 digest is checked when present; the first accepted hash
for each version is recorded and a changed archive for that version is refused. Checking updates is also
an explicit network confirmation. No telemetry or game-folder writes occur in this step.

Only catalogued payload paths are extracted: `version.dll`, `dlssg_sm86.ini` and
`dlssg_sm86/{backend,kernels,runtime,xefg,sr}`. SR is optional for local FG-only imports but is included from
complete official releases because the M/L INT8 SR presets load it at runtime. Legal texts stay separately
in the app's `sm86-releases/<version>/notices` cache. Other safe members, including alternative proxies,
old archives and unknown future extras, are ignored and listed in `ignored-files.log`; unsafe names, links,
collisions, corrupt/truncated archives, excess sizes and missing required payloads are refused.

“Optimize automatically” combines recommendations and a known compatible NR model repair into one review.
Experimental Proton/Wine sm86 requires explicit selection; LSFG is included only when selected and ready.
`Prerequisite::LsfgShaders` starts (or joins) the automatic shader preparation in app data and resumes the
optimization for the same game and facts once the shaders are ready; a failure or a missing Lossless Scaling
folder applies nothing. App-data prerequisites finish before the game
review. Changing the selected game or opening another review invalidates the pending continuation.
The core's reviewed legacy transition backs up/removes a monolithic proxy and INI before new payload copies;
a second sm86 proxy is refused for manual reconciliation. Receipt v1 and running-game refusals still apply.

The game page's one primary button is this optimization, named for what it does there: “Install and optimize…”
for a game without Retoque, “Register and optimize…” for an install an old script made (registered under the proxy
name it already uses, such as `winmm.dll`), “Update and optimize…” when newer Retoque files are active (the update
goes into the same review), “Optimize…” once installed, and “Finish cleanup…” for an incomplete install. It always
installs when Retoque is missing, even if every other item is left out. The review is titled with the clicked label
without “…”, its confirm repeats that verb, and the toast says what was done (“Retoque installed and optimized ·
{game}”). Install or update alone, reinstall, uninstall, undo and the game folder sit in “More actions” beside it.
Tiles under the banner show the graphics card, Retoque, Neural Rendering, frame generation and the game's DLSS
files from facts already inspected.

The page has five tabs: Overview, Settings, Components, Before / after and Logs. Before install, Overview shows only
“What Retoque will do in this game” (the items that one-click review would include, read-only, with the install
options closed inside), plus the health check when it found something and ReShade when the game has it. After an
install under Proton or Wine, the launch option comes first in an accent card whose words follow the store (Steam
launch options; a `WINEDLLOVERRIDES` variable in Heroic or Lutris; any other launcher) until the newest log shows
Retoque loaded; it then stays under “Reinstall options”. ReShade stays folded unless the game has it. Components
groups the NR model, DLSS and frame generation (sm86 and LSFG side by side). The sm86 download is offered only
inside “Install and optimize” or on its card, never at startup, and the home screen has no model offer of its own.

“Before / after” compares the capture pairs in the game's `retoque-captures` folder. Its “Capture now” asks the
running game for a pair without its menu: only while a process holds the game folder, it leaves the runtime's empty
`dlssnr-capture.trigger` beside OptiScaler, waits for the game to take it and for the new pair to land, then shows
it. A request the game does not take within a few seconds (NR off, or a route that never reads it: Vulkan, native
D3D11, the proxy) is removed again, so it cannot fire at a later launch, and the page points to the in-game button,
which is also in the Neural Rendering panel's simple view.

“Last session” on the Overview tab reads the newest log the page already summarized (`retoque_data::session`,
bounded memory): real and on-screen frame rates from the runtime's periodic status lines, measured from the
time between them; the worst 5% of windows; how much of the session frame generation ran and what it multiplied;
the share of drops below 80% of the median; and the real rate over time. Its advice covers a base too low for
frame generation (under about 40 real fps), frequent drops (game streaming or too little free RAM), the
fakenvapi stand-in that hides DLSS frame generation, and NR switched off mid-session. Without status lines in the
log there is no card. Install options (proxy name, NR on, previews) sit closed at the end of the tab; the footer
holds one button naming the Retoque files in use (with “New release available” beside it when there is one), which
opens the “Retoque files” dialog with the payload sources and official releases.

A store's uninstall removes only the files it installed, so a game folder can outlive its game with Retoque's
files in it. Discovery reports unregistered Steam folders with no executable and a Retoque marker
(`retoque_discovery::leftover`); the home screen offers them under “Left behind by uninstalled games”, and the
review lists per folder what goes and what stays. `retoque_core::leftovers` removes only files Retoque, OptiScaler,
sm86, ReShade or their caches put there, never follows links, keeps every other file (saves, store stamps) and
its folder, refuses a folder with an executable, and refuses a folder that changed since the review.

The app's icon is `crates/retoque-app/assets/retoque.svg` (a retouch stroke across coarse pixels): the window
uses its 256 px PNG, and on Windows `build.rs` embeds `retoque.ico` (16-256 px) and the version into the executable
with the MinGW `windres` (`WINDRES` overrides it; without one the build warns and the exe keeps the default icon).
On Linux, `packaging/linux/retoque.desktop` with `Icon=retoque` and `StartupWMClass=retoque` (the window's app id)
goes to `~/.local/share/applications/`, and the PNG to `~/.local/share/icons/hicolor/256x256/apps/retoque.png`.

“Preferences…” in the top bar holds what a game without Retoque starts from (NR on, keep ReShade, the loader file
name, and on Linux the driver's `_nvngx.dll`); a game that already has Retoque keeps its own. It also shows how much
the app's data folder takes. At startup `retoque_data::app_data` clears staging an interrupted import, download or
unpack left there (`.import-…`, `.download-…`, `.unpack-…`, `.prepare-…`, `*.json.tmp`) once it is a day old, in the
data folder and the folders directly inside it only, never following a link; catalogs, models and settings are never
candidates. Games Retoque has been run in are listed, one dated observation per row, in
[docs/games.md](../../docs/games.md).

On RTX 30 (Ampere) two tuning items come from one field session (Dying Light: The Beast, RTX 3060, Windows,
2026-11-10, DLSS-G through sm86 at 4.0x on screen with NR on): with sm86 and a game that ships DLSS-G,
`[FrameGen] FGNvngxReplacement=None` and `[DLSSG] OverrideInterpolationCount=2` (MFG x3; that session's x4 stuttered on
23-27 real frames per second); for a D3D12 game, `[DlssNr] PeripheryCompression=true` and `TransferStrength=1.5`. A
value already set (numbers compare as numbers) is not proposed again, and a frame count the game already has (x4
included) is kept as the user's choice; the "Last session" card still advises x3 on a low base. The card also says to
pick DLSS Balanced or Performance in the game below about 30 real frames per second: the DLSS mode is the game's
setting, which the app does not change. They never choose a frame-generation provider. RTX 20 and RTX 40+ get neither
until a game is measured there; one session is a starting point, not a qualification.

The kit builder can opt into `INCLUDE_OFFICIAL_SM86=1`. The `cache_sm86` example stores the exact archive plus
GitHub metadata in `first-run-cache/sm86`, requiring a published SHA-256 digest. The GUI verifies that cache
again and imports it after confirmation without downloading. `verify_sm86_cache` can exercise that import
against an explicit scratch catalog. These archives/runtimes are never committed. See the additional
[third-party notices](THIRD_PARTY_NOTICES.md); the full upstream package is not covered by its wrapper's MIT license.

## Per-game NVIDIA DLSS updates

The DLSS card downloads NVIDIA DLSS **310.9.16** SR/RR/FG directly from the official `NVIDIA/DLSS` tag after
confirmation. Only a compiled pin table can introduce another version. SHA-256 and exact sizes are enforced for
each DLL and `LICENSE.txt`; legal text, version, source URL and hashes remain in `dlss/310.9.16` under app data.
These tag files are ordinary Git blobs, not LFS pointers. Redirects are restricted to approved GitHub raw/LFS/asset
hosts, HTTPS only. No binaries are bundled or redistributed by Retoque. NVIDIA's separate license applies.

The card reads bounded PE VERSIONINFO resources and shows each discovered path/version. Unknown PE versions stay
unknown. The additive `retoque_discovery::find_dlss_paths` returns all copies under the known game installation,
excluding links, reparse points and loader/catalog directories. Exhausted scan budgets fail closed. The updater
replaces only SR/RR/FG kinds already present, including Unreal plugin paths; newer/equal known versions are skipped.
It never adds a missing frame-generation DLL. “Optimize automatically” includes this only with its optional checkbox
selected (off by default); obtain the catalog first. Doctor reports known older versions as informational findings.

Every game change uses a sealed reviewed plan with regular v1 File/Backup records and the existing pending journal.
If all DLLs being replaced descend from the executable target, its ordinary receipt is reused. Otherwise the
known `install_dir` holds a separate `.retoque/receipt.txt`, with an owned `.retoque/dlss.txt` identity marker.
An unrelated root receipt is refused. Changing receipt roots requires restoring the previous layout first.
Running-game checks cover the whole install root and target, during planning and before each mutation.

The app's uninstall review restores the root receipt before the executable receipt. Partial operations are visible
in Doctor and can be reviewed again. **Legacy Bash/PowerShell uninstallers restore each v1 receipt only when invoked
at that receipt's directory**; uninstalling only the executable folder does not restore a separate root receipt.
A legacy partial uninstall may remove the root identity marker; preserve its receipt/backups and finish that same
legacy uninstall at the root. The app refuses to guess ownership when the marker has been lost or changed.
Concurrent external filesystem races retain the engine limitations documented in the core README.
Anti-cheat/game compatibility is not inferred from successful download or installation; the UI shows a generic
compatibility caution because discovery has no anti-cheat flag. Tests use synthetic files and injected transports,
never real games or network requests, and do not qualify native Windows, Proton, GPU behavior or image quality.

## Official Retoque payload releases (`gabrielmaialva33/retoque`)

The “Retoque files” dialog, opened from the bottom status bar, supports three payload sources:

1. **Beside the app** (`OptiScaler.dll` and `nvngx.dll_dlssnr.dll` next to the executable, in `RETOQUE_SOURCE_DIR`, or in `x64/out` from a checkout)
2. **A chosen folder** picked by the user
3. **Our latest GitHub release** from `gabrielmaialva33/retoque` (`retoque_data::payload_fetch` and `retoque-app::payload_ui`)

Release assets follow [docs/releases/PAYLOAD.md](../../docs/releases/PAYLOAD.md):
`retoque-payload-<version>-x64.zip` (containing `OptiScaler.dll`, `nvngx.dll_dlssnr.dll`, release-configured
`OptiScaler.ini`, `Licenses/`, `translations/` and optional `OptiScaler/`) plus `SHA256SUMS`. Build them locally
from `x64/out` and the repository with:

```bash
bash tools/app/scripts/make-payload-release.sh --build-dir x64/out --out release/payload --version retoque-v0.1.0
```

The script checks the forwarder exports, verifies that `OptiScaler.dll` and `nvngx.dll_dlssnr.dll` have matching
PE `VERSIONINFO` file versions, applies the release INI defaults (`LogToFile=true`, `LogLevel=2`, `ShortcutKey=0x24`,
refusing any `Enabled=true`), enforces the forbidden-file guard (never NVIDIA models, `_nvngx.dll`, `dlssg_for_sm86`,
Lossless Scaling or `.pdb`/`.lib`/`.exp`/`.ilk` files), writes the archive and `SHA256SUMS`, and prints the
`gh release create` command it would run without executing it.

In the app, checking for updates or downloading a release requires explicit confirmation. Network requests use
synchronous `ureq` + `rustls` limited to `api.github.com/repos/gabrielmaialva33/retoque/releases`, that
repository's release download URLs, and GitHub's release asset CDNs (`release-assets.githubusercontent.com`,
`objects.githubusercontent.com`). The archive is verified against both `SHA256SUMS` and the GitHub asset SHA-256
digest, and only allowlisted paths are unpacked into app data (`payload-releases/<version>/payload`). When no
release has been published on GitHub yet, the app states so plainly and keeps the beside-app and chosen-folder
sources. When a newer payload is active, an installed game's page offers “Update and optimize…” (or “Update Retoque…”
under “More actions”), both through the normal reviewed install `Plan`.

## ReShade and add-ons

The game page can prepare **ReShade 6.8.0 with full add-on support** from the official `reshade.me` installer,
with confirmation before HTTPS access. Both installer and extracted x64 DLL have compiled SHA-256/size pins.
The content-addressed app cache reuses the component catalog's capability-relative I/O and keeps the upstream
BSD-3-Clause license. A local DLL or appended-ZIP setup can also be imported; the standard limited build warns
that it cannot load external add-ons. No ReShade binary is committed or bundled.

Retoque keeps its proxy and loads `ReShade64.dll` using `[Plugins] LoadReshade=true`. Installation refuses separate
proxy/executable directories, a remaining ReShade proxy (use the existing `keep_reshade` migration), and Special K.
Replacing a recognized limited companion preserves its original DLL in the ordinary v1 backup receipt. Unknown
companions are refused. Settings undo lives in receipt-owned `.retoque/reshade.ini`; the v1 receipt syntax is unchanged.
Uninstalling this component removes its own DLL or restores the original and reverses the loader setting, preserving
`ReShade.ini`, presets and shaders. Exact INI undo requires the recorded resulting bytes; changed unrelated settings
use a conservative key undo, while a changed loader key refuses restoration.

The add-on list follows `[ADDON] AddonPath` (non-recursive) and the case-sensitive ReShade `DisabledAddons` rules:
comma-separated entries with `,,` escaping and `name@filename` identities. Files are statically inspected, never loaded.
Drops and file selection propose receipt-owned copies, never moves; different same-name files are refused, and
identical preexisting files retain their ownership. Imports into the game root are refused when AddonPath points
elsewhere. Add-on toggles preserve other INI bytes and record sparse undo in `.retoque/addon-<hash>.ini`. Removing
an imported add-on restores its previous disabled state. External files are never removed by the add-on command.
Unresolved Wine drives, linked files, ambiguous casing and scan/size budgets fail closed. A custom AddonPath outside
the watched target needs a manual refresh after outside edits.

Only enabled modules whose filenames contain `renodx-dlss` are NR candidates; HDR-only RenoDX mods never change the
owner. The game page offers `[DlssNr] YieldToAddons=true` (absent/`auto` default) or expert `false`,
writing only after a choice and review. This is configured/static ownership, not proof that ReShade or the add-on
loaded; the runtime side is `[DlssNr] YieldToAddons` in OptiScaler (retoque/dlssnr/DlssNr_Consumers.h).
Doctor offers the same reviewed ReShade repair for missing/limited builds. Automatic optimization never installs
ReShade, even when it recommends it.

```sh
retoque reshade install --dry-run <game-dir> <ReShade64.dll-or-setup.exe>
retoque reshade install <game-dir>                    # cached official build, or explicit download prompt
retoque reshade uninstall --dry-run <game-dir>
retoque addon list <game-dir>
retoque addon add --dry-run <game-dir> <mod.addon64>
retoque addon disable <game-dir> <mod.addon64>
retoque addon enable <game-dir> <mod.addon64>
retoque addon remove <game-dir> <mod.addon64>
```

A setup dry-run stages extraction in a temporary directory, removed on exit; it never changes the game or publishes
the app cache. CLI `--yes-download` explicitly consents to the pinned official download; it does not bypass plan
refusals. Game changes are shown before apply. All operations share stale-plan and running-game guards. Neither
these host tests nor PE inspection qualify GPU behavior, game compatibility, or native Windows operation.

The legacy Bash/PowerShell uninstallers consume the normal v1 file/backup records, but do not replay the new
INI provenance. Use the app or CLI component/full uninstall for exact restoration of ReShade loader/add-on settings.

ReShade's default input mode (`[INPUT] InputProcessing=2`) gives its open overlay all mouse and keyboard input, and
its message hooks see input before OptiScaler's, so the Retoque menu cannot be used while both are open. Installing
the component sets `InputProcessing=1` in ReShade.ini (block only while the cursor is over ReShade's own windows),
with its own undo state; uninstall restores the previous mode unless the user changed it since. For a ReShade that
was already there, Doctor and the ReShade card offer the same reviewed edit. The legacy uninstallers leave that one
setting in place.
