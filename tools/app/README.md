# Retoque app

One installer and manager for Linux and Windows: find games, install, update and remove Retoque, change its
settings per game, choose the NR model per game and read the logs. Design and scope:
[2026-11-05 spec](../../docs/superpowers/specs/2026-11-05-retoque-app.md).

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
the app runs from a checkout, in `RETOQUE_SOURCE_DIR`, or in the folder chosen in the window.

## Rules the app keeps

- Every write is a plan first: the window shows it, and nothing is written while the plan has a refusal.
- A running game is never written to: install, uninstall and settings all refuse while a process holds a file in
  the game folder.
- The receipt is the one tools/deploy-to-game.sh and tools/install-windows.ps1 write; each undoes the other's.
- `nvngx_dlssnr.dll` is never copied into, moved in or removed from a game folder. Models imported into the catalog
  are the user's own files, kept in the app's data folder, chosen with `[DlssNr] ModelFile`.
- Settings are written key by key into the ini on disk, so a change the in-game menu made to another key survives.

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
