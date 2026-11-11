# Field checks, 2026-11-10

Status: **game observations, not qualification.** Two short checks read afterwards from each session's
OptiScaler log (and, for the second, the app's "Last session" card built from that log). No image-quality
judgement and no frame-pacing or latency measurement. Rates are presents and renders per ~2 s status window.

## Cyberpunk 2077 on Proton, RTX 4090

- Arch Linux, the game's configured Proton (GE-Proton11-12), RTX 4090, driver 615.78.08, 3440x1440 165 Hz, VRR off.
- OptiScaler `v10.0.0-dev (1eb4b94b)`, build stamp `20261110_123405`, installed by the Retoque app as `dxgi.dll`
  with a receipt; the game's own `OptiScaler.ini` kept (`[DLSSG] OverrideInterpolationCount=2`).
- NR model: the app catalog's 310.9.16 compat-fp16 build through `ModelFile`. DLSS DLLs 310.9.16 in the game.
- Session 20:45-20:49: title screen, a save continued, walked in the city.

| What | Log |
|---|---|
| Upscaler | DLSS Ray Reconstruction at render = display 3440x1440 (DLAA) |
| NR | `stage=after-rr`, 1,481 model evaluations, 0 failed; adaptive cadence every 2nd-4th frame |
| Frame generation | the game asks for 1 generated frame, the override makes it 2 (3x); `165 presents : 55 renders = 3.00x` |
| MFG unlock | applied to the game's `nvngx_dlssg.dll` 310.9.16.0 (`advertise=ok validate=ok blackwellKernels=ok`); a second copy, 310.2.1.0, did not match and is not the one in use |
| Errors | Streamline `setDynamicMFGParams failed` every 10 s (the dynamic MFG mode, absent on Ada), Reflex sync errors at start (common under Proton); no NR error |

About 27 rendered frames per second at DLAA with RR and NR. Frame generation turns off while OptiScaler's own
menu is open or the window loses focus, and back on after.

## Dying Light: The Beast on native Windows, RTX 3060

- Native Windows 10, RTX 3060 12 GB, driver 621.14. A 2026-11-10 build installed by the app (the session's exact
  build hash was not recorded).
- DLSS-G through the sm86 component (`dlssg_for_sm86` 0.4.1) at 4x, NR on.
- Session 15:39-15:49 (10 min), as the app's "Last session" card read it from the log:

| Real frames | Worst 5% | On screen | Frame generation | Drops |
|---|---|---|---|---|
| 27 fps | 17 fps | 35 fps | x4.0, active 47% of the session | 12% of the session |

NR was turned off for part of the session with its in-game key. With ~23-27 real frames per second the frame
generation stuttered; the card's advice (DLSS Balanced or Performance, or x3 instead of x4) applies. The same
machine reset with Kernel-Power 41 (no bugcheck) several times that day, with and without the game running.

## Limits

One session per game, one machine each. A run that reached play with no NR error is evidence the route works on
that build, not that it looks right or that a later build behaves the same.
