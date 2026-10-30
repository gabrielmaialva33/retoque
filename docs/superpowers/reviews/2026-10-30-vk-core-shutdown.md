# Vulkan core shutdown — runtime evidence, 2026-10-30

Status: **two existing scenarios pass on the fixed DLL with judged receipts; the baseline public path fails.** This
records one Proton/driver environment. It is not native Windows, game, or cross-driver qualification. No commit,
merge, push or deployment is implied. Paths are relative to a user-local `<evidence-root>`; raw logs, binaries and
captures stay outside Git.

## Identities

| Item | SHA-256 |
|---|---|
| Baseline DLL (historical, `combined-binary`; not a clean build of any HEAD) | `63d95a0f8a2a9d8d0ac72bfd0acfd09aae6140491f83fb21bb3d14967bfc32a4` |
| Fixed DLL, build 1 (adapter only) | `97b603c29ce3e83bfab40b3813a7de6f1bce2a65b99c4428c39d493ff405dee2` |
| Fixed DLL, build 2 (+ adapter debug lines) | `74a02e555396e341b50fd4095634848ce59576f2ca6360886e11cdc33f63e12e` |
| Fixed DLL, build 3 (+ public `Shutdown1` debug lines), judged | `a3003b6a8e1f74810274b6cd047923edafbe64a823b2d41d77ebbfd1ba0461be` |
| Forwarder (builds 1–3) | `dc0bdee4eafbaa9c1e7290971e72629a0c256fb9848ab76721d799b5383c4463` |
| Driver core `_nvngx.dll` 32.0.16.1752 (system and prefix copy) | `dabe18b0dc559750113b589d06f672141c8c49a2a571e0a5a7ca7b3e584a7ac3` |
| SR `nvngx_dlss.dll`, user-owned, linked into the route case only | `3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983` |
| NR model | `6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927` |
| `vk-present-host-nr.exe` harness (built once, reused) | `3b595823d13b342d…` (full hash in `runtime-after-3/record.json`) |

Builds 1–3 were made from base `5faa8dce` plus this branch's uncommitted diff, with `CL_MPCount=4`; each build's
source snapshot before and after is retained (`build/`, `build-2/`, `build-3/`) and was unchanged during the build.
Runs used GE-Proton11-11 on an RTX 4090 with `WINEDEBUG=-all,+loaddll`, under the shared compiler/GPU lease.

## Scenarios and results

`nr-control` exercises the NR present host's `ShutdownPresentCore` (no application NGX). `nr-route-quiet` exercises
the public `NVSDK_NGX_VULKAN_Shutdown1`; it only reaches the core with DLSS on, i.e. with `RETOQUE_VK_ROUTE_SR`
linking the SR beside the DLL (without it OptiScaler logs `nvngx_dlss.dll not found, disabling DLSS`).

| Run | DLL | SR | `nr-control` | `nr-route-quiet` | Judge |
|---|---|---|---|---|---|
| `runtime-before-1` | baseline | no | runner PASS | runner PASS, but DLSS disabled: public calls never reached the core | pre-hardening judge; **INCONCLUSIVE, do not claim** |
| `runtime-before-2` | baseline | yes | runner PASS (host path already compensated) | **FAIL: exit 5**; its `result.json` stayed `RUNNING` and the log ends after `TryDestroyNGXParameters`, at the application's `Shutdown1` | old receipt format; retained as baseline FAIL |
| `runtime-after-2` | build 3 | yes | runner PASS | runner PASS | **not judged**: the scratch executor failed before writing receipts; artifacts and `ATTEMPT-NOTE.txt` retained |
| `runtime-after-3` | build 3 | yes | runner PASS, exit 0 | runner PASS, exit 0 | **host PASS** (device `0x76B0C0`), **public PASS** (device `0x76AF90`), run `ff37c756c69c49268495486b05ebf592` |

In `runtime-after-3`, both cases logged their order on the device:

- `nr-control`: `InitVulkan result: 1` → `ShutdownVulkanDevice` entry → `core result 0x1, 0 remaining` → the host's
  shutdown line → device destroyed.
- `nr-route-quiet`: `VULKAN_Init result: 1` → `NVSDK_NGX_VULKAN_Shutdown1` entry → adapter entry → `core result 0x1,
  0 remaining` → `returning 0x1` → device destroyed.

The receipts (`<case>/exit-receipt.json`) hold the run id, case folder, Proton prefix, the application's own return
code and the hashes of facts, OptiScaler log, loader trace and runner results; the archived copies match those hashes
and `runtime-after-3.SHA256SUMS` lists the folder.

## Validation layer

Khronos validation with synchronization checks was active in every judged case (`validation_active: true`). The
runner separates the host's reports from the NR model's own, and the judge only accepts zero of the host's:

| Run | Case | Host's validation errors | Model's own messages, set aside by the runner |
|---|---|---:|---|
| `runtime-after-3` | `nr-control` | 0 | 2 × `VUID-vkCmdDraw-None-09600` |
| `runtime-after-3` | `nr-route-quiet` | 0 | 6 × `VUID-vkCmdDraw-None-09600` |
| `runtime-before-2` | `nr-control` | 0 | 2 × `VUID-vkCmdDraw-None-09600` |
| `runtime-before-2` | `nr-route-quiet` | not written (process died) | not written |

The model's `VUID-vkCmdDraw-None-09600` messages are present in baseline and fixed runs alike; they are not caused or
cleared by this change, and this record does not claim a validation-clean Vulkan run overall.

## Limits

- One driver core, one Proton, one GPU, two synthetic scenarios. Native Windows, games and other core versions are
  not covered.
- The public SDK return is `NVSDK_NGX_VULKAN_Shutdown`'s, not the core's; the core's result is established only by
  the adapter's debug line in this run.
- The baseline public failure has no `+seh` trace; its location is inferred from where the log ends and from the
  exit code, matching the D3D11 crash shape.
- `RETOQUE_VK_ROUTE_SR` is opt-in; the default `nr-route-quiet` run still has DLSS disabled and does not exercise
  the public core call.
