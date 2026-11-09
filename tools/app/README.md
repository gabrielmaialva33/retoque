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
