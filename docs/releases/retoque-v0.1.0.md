# Retoque v0.1.0 (pre-release): desktop app

Retoque brings DLSS Neural Rendering (NR), Lossless Scaling frame generation and Proton fixes to games through
OptiScaler. This pre-release adds the **Retoque desktop app** (Windows and Linux): one install engine shared by
the app and the `retoque` CLI, with every change shown in a review before anything is written to a game.

**Draft for review. Not qualified on games, MFG x3/x4 or native Windows yet** (see "Validation" below).

## Assets

| File | What it is |
|---|---|
| `retoque-app-0.1.0-windows-x64.zip` | The app and CLI for Windows, with Microsoft's DXC compiler (for LSFG shaders) and licences |
| `retoque-app-0.1.0-linux-x64.tar.gz` | The app and CLI for Linux (DXC is offered as a verified download on first use) |
| `retoque-payload-0.1.0-x64.zip` | Retoque itself: `OptiScaler.dll`, the NR forwarder (both 10.0.0.1), release `OptiScaler.ini`, licences, translations |
| `SHA256SUMS` | Checksums of the three files above |

Nothing from NVIDIA, Lossless Scaling or dlssg_for_sm86 is in these files.

## What the app does

- Finds games on Steam, Heroic, Lutris, Epic, GOG, Xbox and your own folders, on every disk, with covers.
- Installs, updates and removes Retoque per game with a reviewed plan, a receipt and backups; refuses while the game
  runs; compatible with the receipts of `tools/deploy-to-game.sh` and `tools/install-windows.ps1`.
- Gets what each game needs, from its official source, after you confirm and with the SHA-256 checked:
  NVIDIA DLSS SR/RR/FG 310.9.16 (official NVIDIA/DLSS repository; per-game update with restorable backups,
  including Unreal plugin folders), dlssg_for_sm86 for RTX 20/30 (official sdli1995 release), Microsoft DXC,
  and Retoque updates from this repository's releases.
- DLSS NR model: offers a model that runs on your GPU from the community catalog (RankFTW/rhi-repo, **not an NVIDIA
  source**), and warns when a model has no kernels for your GPU (the official 310.9.16 model runs only on RTX 50).
- Prepares the Lossless Scaling shaders from your own `lsfg-vk.dll` inside the app (byte-identical to the
  reference converter), never copying or redistributing them.
- "Optimize automatically": component, settings, NR model, LSFG and optional DLSS update in a single review.
- Doctor with evidence and reviewed repairs, log diagnosis, live refresh of the selected game, Ctrl+K command
  palette, fuzzy search, settings filter and undo, pt-BR and English.

## Also in this build

- OptiScaler upstream `optiscaler/master` 34d492d6 (77 commits) merged, with the 8 findings of an independent review
  fixed (Vulkan device teardown, NGX parameter lifetime, D3D11 bridge presenter, FG fences and queues).
- Proton: per-thread Vulkan swapchain guard (F11/Alt-Tab), one Reflex update per present, no FG frame-count
  flapping from async/RTSS markers. The NR forwarder now carries a version resource.

## Validation

Host tests 134/134 (C++) and 323 (app), Release build, Wine: NR loopback, D3D11 guides 8/8, scene 10/10, Vulkan
34/37 then 3/3 on retry. Not yet: game route matrix, MFG x3/x4, native Windows runtime. Records:
`docs/linux-proton/upstream-20261109.md`, `docs/releases/PAYLOAD.md`.
