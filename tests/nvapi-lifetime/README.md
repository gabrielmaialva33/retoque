# nvapi64.dll lifetime after NvAPI_Initialize

```bash
python3 tests/nvapi-lifetime/run.py
```

Python only, well under a second. Source slices of the two startup NVIDIA checks, comments stripped:
`IdentifyGpu::queryNvapi` (`OptiScaler/misc/IdentifyGpu.cpp`) and the `isNvidiaViaNvapi` lambda in
`OptiScaler/dllmain.cpp`.

Adapted from janblade/OptiScaler-F5-DLSSNR-Multipass `d2c06938` (GPL-3.0). Evidence there: a BeamNG.drive (Vulkan)
crash dump, an access violation on a thread-pool thread inside the already unloaded `nvapi64_impl.dll`.
`NvAPI_Initialize` leaves driver work scheduled in that module; in a game that never loads nvapi itself, the
startup check's `NvAPI_Unload` + `FreeLibrary` dropped the last reference. Record:
[nvapi-keep-loaded-20261030.md](../../docs/linux-proton/nvapi-keep-loaded-20261030.md).

## What it checks

- `queryNvapi`: once `init()` has been called (success or failure) there is no `FreeLibrary_Ldr(nvapiModule)` and no
  `NvAPI_Unload`; a failed initialise returns early; the paths before it (no `nvapi_QueryInterface`, fakenvapi, no
  `NvAPI_Initialize` export) still free the module.
- `isNvidiaViaNvapi`: exactly one `init()`, no `NvAPI_Unload` anywhere, the initialising branch contains no
  `FreeLibrary_Ldr` and ends in `return nvidiaDetected;`, and the shared `FreeLibrary_Ldr` after it (dxvk-nvapi,
  fakenvapi) remains.

Mutation-checked on 2026-10-30: restoring the trailing `FreeLibrary` in `queryNvapi`, freeing on a failed initialise,
and dropping the early return in the lambda each fail it.

## What it does not cover

It executes nothing. That the crash no longer happens, and that installing the nvapi hooks at startup on native
NVIDIA (a side effect: nvapi is now already loaded when `CheckWorkingMode` looks for it) changes nothing a game
notices, need a native Windows run of a Release build.
