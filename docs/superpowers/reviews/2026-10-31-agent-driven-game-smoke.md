# Agent-driven game smoke runs, 2026-10-31

Status: **game observations, not qualification.** Four installed games were started, driven into play and closed
by an agent with nobody at the machine, to see that the current build loads, takes its NR route and reports the
model's VRAM in real games. One short session per game; no image-quality judgement, no performance claim beyond the
numbers below, no native Windows.

## Build and machine

- OptiScaler `2da82e81` (DOOM Eternal also on `cf4b4db3`), forwarder from the same build, installed with
  `tools/deploy-to-game.sh` (receipts, each game's own `OptiScaler.ini` preserved).
- RTX 4090, driver 615.78, GE-Proton11-11 or each game's configured Proton, Hyprland (Wayland, XWayland games).
- Models are each game's own `nvngx_dlssnr.dll` (310.9.16.0 where read; DOOM's through the kit).

## How the games were driven

- Launch: `steam -applaunch <appid>`, with per-launch arguments when needed (Cyberpunk `-skipStartScreen
  -benchmark`); nothing written to Steam's configuration.
- Input: Linux `uinput` devices created for the moment through python-evdev (the user's session has access to
  `/dev/uinput`): a keyboard, a relative mouse, and an Xbox 360 pad (`045e:028e`). Before every input the game's
  window was focused and the input refused unless it was the active window. `wtype` did not work: XWayland games saw
  every key as the same one. DOOM Eternal's load screen accepted only the pad; Starfield's title screen did not see the
  pad and took Enter.
- Reading: window screenshots (`grim`), the game's OptiScaler log, and for Cyberpunk the benchmark's own results.
- Exit: the window's close request (`hl.dsp.window.close`), never a kill. Every game exited.
- Where a game's INI was changed for a run (Cyberpunk's timing and scale keys, The Witcher 3's `LogToFile`), a byte
  copy was kept and restored afterwards, and the hash compared.

## Results

| Game | API | NR route | What ran | Model VRAM (own count) | OptiScaler errors |
|---|---|---|---|---:|---:|
| DOOM Eternal | Vulkan | Vulkan present host (cold NR) | title screen, menus, a save continued and walked; ~83,000 presents | 345 MiB at 841x671 | 0 |
| Cyberpunk 2077 | D3D12 | after Ray Reconstruction, 3440x1440, DLSS MFG | built-in benchmark, about 20 runs (timing, scales, A/B, captures, a replay clip) | 578 MiB at working scale 1.0 | 0 |
| The Witcher 3 (DX12) | D3D12 | after Ray Reconstruction, 3440x1440, FG ~3x | intro skipped, save continued, walked in Novigrad | 313 MiB (working scale 0.5) | 0 |
| Starfield | D3D12 | after upscale (DLSS Quality, guides 2292x960), FG 2x | shader compile, save continued, walked in Cydonia | 578 MiB at working scale 1.0 | 0 |

"OptiScaler errors" excludes Streamline's own log lines forwarded at error level (OTA updater, Reflex timing), present
before this build. The same model and scale gave the same bytes across games: 578 MiB at 1.0 in Cyberpunk and
Starfield, 313 MiB at 0.5 in The Witcher 3 and in Cyberpunk's scale sweep.

The measurements taken through the Cyberpunk benchmark are recorded where they belong:
[model cost by scale and the outside-the-model A/B](../../dlssnr/design/model-cost-vs-working-scale.md),
[the cadence fade change](../../dlssnr/design/cadence-smoothing.md),
[the model's VRAM figure](../../dlssnr/design/model-vram-stats.md) and
[a second working-scale retention clip](../../dlssnr/design/working-scale-detail-retention.md).

## Limits

- One short session per game, one machine; no D3D11 bridge, native D3D11 guides or no-upscaler present title in this
  set (the route matrix in [maintenance](../../maintenance.md#validation-route-matrix) lists them).
- Driving by screenshots and fixed inputs reaches play, not difficult play: no combat, no long sessions, no menus
  beyond what loading a save needs. OptiScaler's own overlay opened in The Witcher 3 but could not be navigated by
  the mouse there (the game holds the pointer).
- No image-quality statement. A run that reached play with no error is evidence the route works, not that it looks
  right.
