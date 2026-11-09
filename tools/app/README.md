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
`autotune::optimize::Prerequisite::LsfgShaders` is the integration hook for shader preparation; this build
reports it unavailable and does not apply partial settings. App-data prerequisites finish before the game
review. Changing the selected game or opening another review invalidates the pending continuation.
The core's reviewed legacy transition backs up/removes a monolithic proxy and INI before new payload copies;
a second sm86 proxy is refused for manual reconciliation. Receipt v1 and running-game refusals still apply.

The kit builder can opt into `INCLUDE_OFFICIAL_SM86=1`. The `cache_sm86` example stores the exact archive plus
GitHub metadata in `first-run-cache/sm86`, requiring a published SHA-256 digest. The GUI verifies that cache
again and imports it after confirmation without downloading. `verify_sm86_cache` can exercise that import
against an explicit scratch catalog. These archives/runtimes are never committed. See the additional
[third-party notices](THIRD_PARTY_NOTICES.md); the full upstream package is not covered by its wrapper's MIT license.
