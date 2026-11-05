# DOOM Eternal Vulkan runs, 2026-11-05

Status: **game observations on one machine, not qualification.** Three short sessions of DOOM Eternal (native
Vulkan, OptiScaler's own DLSS) to check the 2026-11-05 NVIDIA 310.9.16 DLLs, the native Vulkan comparison capture
and native Vulkan NR before the upscaler (`Stage=1`). Read afterwards from each session's OptiScaler log and from the
captured files. Frame rates are presents per second between log milestones, not a frame-pacing measurement.

## Machine, game, DLLs

- Arch Linux, GE-Proton11-12, RTX 4090, driver 615.78.08, 3440x1440 165 Hz, VRR off. DLSS Quality: render
  2293x960, output 3440x1440. `[DlssNr] Enabled=true`, `VulkanPresentHost=true`, otherwise the user's ini.
- `nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll`: NVIDIA DLSS SDK v310.9.16 (blob hashes equal to the
  `NVIDIA/DLSS` tag). `nvngx_dlssnr.dll`: 310.9.16.0, replacing a community 310.8.2 build that day.

| Run | Build | Stage | Session |
|---|---|---|---|
| A | `6b44efc6`, `OptiScaler.dll` sha256 `fab6058c…` | auto | 12:05:59-12:10:51 |
| A' | `6b44efc6` + this branch's capture change, sha256 `1c42de09…` | auto | 12:22:54-12:24:30 |
| B | same as A' | 1 | 12:29:30-12:34:30 |

## Observed

- **DLLs.** `DLSS v310.9.16 loaded` in every run. The model initialised (578 MiB at 3440x1440, 1623 MiB with
  Enlarge 2x, which was switched on and off once in run A). No `[E]` line, no device lost, no NGX failure. DOOM's
  log never has a shutdown line (same in an October log); Proton's log shows only `DBG_PRINTEXCEPTION_C` and
  thread-name exceptions in the game process.
- **Placement at Stage auto.** The Vulkan present host runs NR on zero guides in the menus; when the game's DLSS
  feature evaluates, the after-upscale route claims the model and the host stands down. After-upscale NR then
  runs at 3440x1440 with the game's depth and motion (2293x960), linear HDR input (`VK_FORMAT_B10G11R11_UFLOAT_PACK32`,
  format 122), exposure from the game's texture.
- **Comparison capture, run A: refused.** The button said "This frame's format is not one the capture can write
  yet": `VkPairCapture::PlanFor` admitted formats 37, 44, 97 and 109 only, and DOOM's DLSS output is 122.
- **Comparison capture, run A': written.** With 122 admitted under the float rule, two pairs:
  PFM 3440x1440, `linear_hdr=1`, `completion=vulkan-recording-retired+executed-marker`,
  `pair_execution=last-complete-execution-before-retirement`, no non-finite values. Read offline: luminance
  correlation 0.973 and 0.9997 between original and processed, tonemapped PSNR 34.9 and 36.8 dB, more than 5%
  relative change on 72-75% of pixels, the same frame by eye. The pair is on disk 5-6 s after the press (two 59 MB
  files written off the frame).
- **Stage=1, run B: never admitted, silently.** No line about Stage 1 in the whole session: every refusal path of
  `ScopedPreUpscaleVk` returned without a log line, and a selected Stage 1 never falls back, so the after-upscale
  route never ran either. Only the present host's cold NR ran (zero guides, synthesized motion), and the capture
  button was disabled. The reason is not known from this run; the swapchain is `EXCLUSIVE`, which makes the
  exclusive-image refusal the likely one, unconfirmed.
- **Stage=1 cost.** With the command observer active, about **39 presents/s** in play (12:29:55-12:31:55). The
  player switched NR off and on at 12:32:40; switching it off makes the observer blind for the session
  (`DlssNr_VkPreRuntime.cpp`, `Active()`), and from then on about **120 presents/s** (12:32:48-12:34:19). The
  present host's NR pass measured the same before and after (mean 5.07 and 4.81 ms GPU). Run A's present host
  counted about 140 presents/s, menus included. The observer takes one global mutex in every one of its 47
  hooked commands (`DlssNr_VkPreObserve.h`), on DOOM's parallel recording threads.

## Changes made from these runs (branch `fix/vk-capture-b10g11r11-20261105`)

- The Vulkan capture admits `VK_FORMAT_B10G11R11_UFLOAT_PACK32` with the RGBA16F/RGBA32F rule; packed 10-bit
  UNORM stays refused ([design](../../dlssnr/design/nr-vk-pair-capture.md)).
- Every refusal of the Vulkan Stage 1 scope now names itself, and `ScopedPreUpscaleVk::Report` logs each outcome
  (each refusal reason, and the first bound frame) once per session.

## Not established

- Why Stage 1 refuses DOOM. Next run with the logging above gives the reason.
- Whether Stage 1 can be worth its observer cost in any game; the observer needs per-command-buffer state before
  it is usable in a game that records in parallel.
- Image quality of NR in DOOM; the captures are for that, and nothing has been judged here.
