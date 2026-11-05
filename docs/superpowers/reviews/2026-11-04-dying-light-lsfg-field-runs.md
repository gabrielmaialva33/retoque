# Dying Light field runs on two machines, 2026-11-04

Status: **game observations, not qualification.** Players played Dying Light (2015, D3D11, no upscaler of its own)
for hours on two machines with the same build. Read afterwards from each session's OptiScaler log. No image-quality
judgement, no frame-pacing or latency measurement.

## Build and settings

- OptiScaler `690916b1` (source `651ef627`, clean, build stamp `20261104_115853`), `OptiScaler.dll` MD5
  `ecc08d209a44af57255f0f8adb3c82b3`, installed as `dxgi.dll` (one proxy in the folder); forwarder
  `nvngx.dll_dlssnr.dll` MD5 `ab369adc46955765f6e8b0cbd4924d8a`. Model: each install's own `nvngx_dlssnr.dll`,
  310.9.16.0.
- Same settings on both: `[DlssNr] Enabled=true`, `HookMethod=2` (present), `SynthMotion=true`; `[FrameGen]
  Enabled=true`, `FGInput=Synthesized`, `FGOutput=fsrfg`, `SynthesizedMotion=true`; `[LSFG] Enabled=true`,
  `Multiplier=2`, the user's own shaders from a cache beside the proxy. Route: D3D11 bridge (`Dx11wDx12`
  swapchain), NR at present on the bridge's D3D12 device.
- As `winmm.dll` the same proxy never saw the swapchain on machine B (11:42-11:47): the game's renderer DLL imports
  `dxgi`/`d3d11` statically. Renamed to `dxgi.dll`, it did.

| | Machine A | Machine B |
|---|---|---|
| OS / runtime | Arch Linux, GE-Proton11-12 | native Windows (`Wine not detected`) |
| GPU / driver | RTX 4090, 615.78 | RTX 3060, 617.14; a Parsec virtual display adapter present |
| Output | 3440x1440, 165 Hz, VRR off | 1920x1080 |
| Session read | 16:14-18:13 (1 h 59) | 13:04-15:28 (2 h 24, still playing when read) |

## Observed

- **LSFG takeover of FSR-FG.** Both: FSR-FG context created, then `LSFG: takes over FSR-FG's interpolation` 5 s
  (A) and 4 s (B) after the swapchain; never handed back to FFX after the first seconds, `0 not ready, 0 refused`
  in every 10 s summary. Generated frames per second: A ~80 (about 160 shown; `GPU p50` 0.79 ms per generated frame,
  steady over the session); B ~42 (about 84 shown; 2.0-3.6 ms, varying with the scene, not rising with time).
- **NR at present.** Both: the model ran on every base frame handed to it (A 564 750 of 564 750). `WorkingScale=auto`
  resolved to 1.00 on the 4090 and 0.50 (960x540 working size) on the 3060. NR dispatch, GPU: A ~4.3 ms,
  B ~10.3 ms per base frame, flat over the session. On B that is about 43 % of a ~24 ms base frame.
- **Display-only adapter filtering** (commit `99945d24`, roadmap "validation before more runtime changes"): on B
  `IdentifyGpu` logged `Skipping NVIDIA GeForce RTX 3060: a display-only adapter (indirect display, no rendering)`
  for the Parsec adapter's LUID, and NR/LSFG ran on the real GPU. The only `[E]` line on B, `IdentifyGpu: Failed to
  create DXGI Factory`, came before it and changed nothing visible.
- **Exit.** A: the game exited normally at 18:13:07 (Steam's "game stopped", no Wine error). B: still running.
- **OptiScaler menu with LSFG.** In an earlier session on A that day (13:01-14:45, same build; that log was
  overwritten by the next launch and is not kept), the menu was opened and closed three times and LSFG kept
  generating after each. A morning report of frame generation not resuming after the menu (earlier build) was
  therefore not reproduced; it remains unexplained, not fixed.

## Not a game reset: synthesized-motion scene cuts

The NR log line `the game asked for a history reset` reached about 2 200 on A and 900 on B. Dying Light has no
upscaler and passes NR no reset; on this route the reset comes from the synthesized-motion estimator's scene-cut
bit (`SynthMotionGuide::Record`, "a cut is a cut, whoever notices it"), which the log counts under the game's name.
LSFG's `held (warm-up/reset)` frames, 1-5 % of a 10 s window, follow the same cuts. Whether these are real cuts or
false positives under fast camera motion was not checked against the picture.

## Limits

One game, one route, two machines. No screenshots or captures, no comparison with NR or LSFG off, no pacing or
latency measurement, no native D3D12 or Vulkan route. Machine B's numbers come from a copy of a live log.
