# nvapi64.dll stays loaded after the startup checks (2026-10-30)

A fork fix in two upstream files, adapted from janblade/OptiScaler-F5-DLSSNR-Multipass
[`d2c06938`](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/commit/d2c06938) (GPL-3.0).

**Evidence (from that fork):** a BeamNG.drive crash dump, Vulkan, an access violation on a thread-pool thread inside
the already unloaded `nvapi64_impl.dll`. OptiScaler's two startup NVIDIA checks, `isNvidiaViaNvapi` in
`OptiScaler/dllmain.cpp` and `IdentifyGpu::queryNvapi` in `OptiScaler/misc/IdentifyGpu.cpp`, each loaded
`nvapi64.dll`, called `NvAPI_Initialize`, then `NvAPI_Unload` and `FreeLibrary`. `NvAPI_Initialize` leaves driver work
scheduled in `nvapi64_impl.dll`; in a game that never loads nvapi itself, that dropped the last reference.

**Change:** once `NvAPI_Initialize` has been called, successful or not, neither check calls `NvAPI_Unload` or frees
the module. The paths that never initialise it (no `nvapi_QueryInterface`, dxvk-nvapi in the dllmain check,
fakenvapi in system32, no `NvAPI_Initialize` export) still free it. The module's reference count grows by one per
check that initialised it; nothing else changes in either function.

**Side effects:**

- On native NVIDIA, `nvapi64.dll` is already loaded when `CheckWorkingMode` asks `GetDllNameWModule(&nvapiNamesW)`,
  so `LibraryLoadHooks::LoadNvApi` installs the nvapi hooks at startup instead of when the game loads nvapi. Most
  games got the same hooks anyway; a game that never uses nvapi now carries inert detours.
- Under Wine/Proton the dllmain check never initialises dxvk-nvapi (unchanged), but `queryNvapi` does, so dxvk-nvapi
  now stays loaded and initialised for the process after GPU identification.
- `queryNvapi` also reads the driver version now, for the NR driver gate
  ([driver-gate.md](../dlssnr/design/driver-gate.md)).

**Evidence here:** `tests/nvapi-lifetime` (source slices; three mutations killed). Not built with MSVC, not run on
Windows or in BeamNG.drive; the crash and the startup-hook side effect need a native Windows run of a Release build.
