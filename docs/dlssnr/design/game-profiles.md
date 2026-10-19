# Game profiles: the keys a game needs, applied without the ini

Status: built and host-tested on `claude/game-profiles-20261019` (2026-10-19, from `2af53d2c`), and measured in two
games on 2026-10-19 with every route key at `auto` (see "In game" below).

## The problem

With `[DlssNr] Enabled=true` and every other key at `auto`, several games run nothing at all, and the reason is in
none of the logs a player reads. Each of these was found the hard way and is written down, but only as a key the user
has to put in their ini:

| Game | Exe | What it needs | Why | Measured in |
|---|---|---|---|---|
| Divinity: Original Sin 2 | `EoCApp.exe` | `HookMethod=2` | D3D11 with no upscaler; the bridge host is decided at swapchain creation, and only on an explicit 2 | [nr-dx11-bridge-host.md](nr-dx11-bridge-host.md), 2026-09-18 |
| Divinity: Original Sin 2 | same | `[Libraries] NvapiPath` = Proton's `nvapi64.dll` | Proton lists the game under `disablenvapi` and deletes `nvapi64.dll` from the prefix; NGX then fails to build the model's kernels (`nvapi status -3`, `FAIL_PlatformError`) | CLAUDE.md, "Titles with no upscaler" |
| Generation Zero | `GenerationZero_F.exe` | `HookMethod=2` | D3D11, no upscaler | [nr-dx11-bridge-host.md](nr-dx11-bridge-host.md) |
| Sons Of The Forest | `SonsOfTheForest.exe` | `HookMethod=2`, `[Upscalers] Dx11Upscaler=dlss` | D3D11 with native DLSS: the present pass on the bridge takes DLSS's own depth and motion only on that pair | [dx11-native-dlss-guides.md](dx11-native-dlss-guides.md), measured |
| Hellraiser: Revival | `Hellraiser-Win64-Shipping.exe` | `HookMethod=2`, `ZeroGuideReset=true` | video and UI drawn after the upscaler; the present pass covers them | [hellraiser-video-present.md](hellraiser-video-present.md) |
| DOOM Eternal | `DOOMEternalx64vk.exe` | `VulkanPresentHost=true` | native Vulkan ignores `HookMethod`; with the game's DLSS off only the present host runs the model | [nr-vulkan-present-host.md](nr-vulkan-present-host.md) |
| PCSX2 | `pcsx2-qt.exe` | `HookMethod=2` | D3D12 with no upscaler | README "Status" |

`HookMethod=auto` means 1, not numeric Auto (`Config.h`, [nr-present-hook.md](nr-present-hook.md)), and numeric Auto
cannot pick the bridge host anyway, because that decision is made before any upscaler has been called
(`dx11_with_dx12_sc.cpp`). So a general rule cannot replace this list today; a per-game one can.

## What exists to build on

Upstream's `misc/GameConfigs` matches the game by lowercase exe name (plus `<name>-win64-shipping.exe`), and its
`.defaults` apply a value only when the user has not set the key, through `CustomOptional::set_volatile_value`: the
value is used for the session and never written back by a menu save (`value_for_config` returns the ini's value). No
`DlssNr` member is in its table, and the table is upstream's file, edited often, so ours goes beside it.

## Design

**`retoque/dlssnr/DlssNr_GameProfiles.h`** (header-only, no `<windows.h>`, host-tested): the table above and
`Match(exeName, exeDirectory)`, which returns the profile for this process or none. Exe names compare without case;
an optional directory condition exists for games that ship one exe name in two renderers (none in the table today).

**`DlssNr_GameProfiles.cpp`**: `Apply(exePath)`, called once from `CheckQuirks` in `dllmain.cpp`, right after
upstream's `ApplyGameConfigs` (a one-line call site, like the module's others). It:

1. does nothing unless `[DlssNr] Enabled` is true at start-up and `[DlssNr] GameProfiles` is not false. With NR off
   nothing changes, so the module stays default-identical when off (`DEVELOPMENT.md` §1). The bridge host and the
   Vulkan present host are both decided at start-up, so a profile applied later could not take effect anyway; turning
   NR on in the menu mid-session still needs a restart for them, as it does today.
2. sets each key of the matched profile with `set_volatile_value`, **only where the key has no value**. `auto` in the
   ini reads as no value (`readString` turns it into `nullopt`), so the shipped ini lets the profile in, and any
   explicit value in the user's ini (a number, `true`, `false`, a path) wins.
3. logs one line: `DLSS-NR game profile: Divinity: Original Sin 2 (EoCApp.exe): HookMethod=2 (from the profile),
   NvapiPath=Z:\...\nvapi64.dll (found in Proton)`, naming each key it set and each it left to the user.
4. records the profile's name and keys for the menu, which shows `Game profile: Divinity: Original Sin 2 -- hook method
   2` under the status line. The keys stay editable; the menu's own edits are ordinary assignments and clear the
   volatile flag, as they do for upstream's defaults.

**Proton's `nvapi64.dll`, found rather than configured.** When NR is on, the process runs under Wine, `NvapiPath` has
no value and `system32\nvapi64.dll` does not exist, `Apply` looks in the `WINEDLLPATH` the Proton script exports
(`<proton>/files/lib/vkd3d:<proton>/files/lib/wine[:...]`, read in GE-Proton11-10's `proton`, line 2072) for
`<entry>/nvapi/x86_64-windows/nvapi64.dll`, and sets `NvapiPath` to that file as `Z:\...` (volatile). This is general,
not per-game: any title Proton denies NVAPI to has the same failure. It stays scoped to NR being on, and to the file
being absent from the prefix, so a prefix where Proton installed NVAPI is never touched. Loading it only gives
OptiScaler (and NGX through it) the real NVAPI, as the hand-set `NvapiPath` did in the measured Divinity run.

**`[DlssNr] GameProfiles`**: `auto` (true) or `false`. Four-point config round trip (`Config.h`, read, save, the
shipped ini), checked by `nr-invariants`.

## Left out, on purpose

- No profile turns NR on. Enabling stays the user's (or the installer's) choice.
- No frame-generation, LSFG, MFG or look settings: those are preferences, not what makes a game run.
- No entry without a measured run here. The Witcher 3's DirectX 11 renderer needed `HookMethod=2` on 2026-09-18, but
  its `bin\x64` holds no executable on this machine any more (only `bin\x64_dx12\witcher3.exe`, which must not get
  it), so it waits for a measurement.
- Crimson Desert: it runs after Ray Reconstruction with every key at `auto`. Its `HookMethod=2` (kept in the ini
  here) moves the pass to the game's Present, ahead of its DLSS-G ([nr-before-dlssg.md](nr-before-dlssg.md);
  the log says `present: at the game's own Present, ahead of its frame generation`), which is a choice about where
  NR sits relative to frame generation, not something the game needs before NR can run, so it is no profile.

## Tests

`tests/nr-game-profiles` (host): exe matching (case, `-Win64-Shipping`, a near miss), the directory condition, the
apply rules against a fake option type with `has_value`/`set_volatile_value` (user value wins, `auto` lets the profile
in, NR off changes nothing, `GameProfiles=false` changes nothing), every profile key being a real config member, and
the `WINEDLLPATH` search (Unix to `Z:` paths, entries without `/wine`, a missing file, a present `system32` copy).
In game: Divinity, Sons Of The Forest, DOOM Eternal and Generation Zero with their keys removed from the ini.

## In game, 2026-10-19

Build `cd568080` (this branch merged with the ViT-reuse experiment, which stayed off), GE-Proton11-10, RTX 4090,
the inis regenerated from the shipped template with the route keys at `auto`:

- **Divinity: Original Sin 2**: `HookMethod=2 (from the profile)`, and `the prefix has no nvapi64.dll;
  NvapiPath=Z:\...\GE-Proton11-10-x86_64\files\lib\wine\nvapi\x86_64-windows\nvapi64.dll (found in Proton, this
  session only)`. NGX then reported native NVAPI, built the 310.9.16 model, and the present host ran it on 3745 of
  3746 base frames with synthesized motion, beside LSFG x4.
- **Sons Of The Forest**: `HookMethod=2 (from the profile), Dx11Upscaler=dlss (from the profile)`. Once the game's
  DLSS started, its depth and motion were carried to the bridge's present pass on 8177 of 8177 evaluates, and the
  model ran on 10209 of 10210 frames. Before that (menus, loading) the pass ran on zero guides, as designed.
- **Crimson Desert** (no profile): no profile line, as expected.

DOOM Eternal, Generation Zero, Hellraiser and PCSX2 were measured with the same keys set by hand before this change;
their profile entries have not yet been run through `Apply`.
