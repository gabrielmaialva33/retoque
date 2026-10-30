# NR minimum NVIDIA driver gate

Status (2026-10-30): implemented and host-tested (`tests/nr-driver-gate`). **Not yet built with MSVC or run on native
Windows**; nothing here has been observed on a driver below 610.88.

## Evidence

DLSS Unlocked issue #60: an RTX 2080 Ti on Windows driver 576.88. NGX refused feature 18 with `FAIL_OutOfDate`
("please update display driver"), a loader that calls the model itself then created it anyway, and the GPU hung until
a reboot. ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass answered it in `97f35314` (version gate), `521138df` (menu and
hotkey) and `dec66857` (no direct-runtime fallback after `FAIL_OutOfDate`), GPL-3.0. 610.88 is the oldest driver that
fork verified running NR; 616.64 is the first with official support ([ngx-driver-616.md](ngx-driver-616.md)).

## Why Retoque's shape differs

In that fork the proxy asks the driver's NGX for feature 18 first and falls back to the direct runtime on failure, so
"do not fall back after `FAIL_OutOfDate`" is one branch. Retoque has no automatic fallback (`DlssNr_Dx12.cpp`: "Nothing
falls back automatically"), and its two loaders, the forwarder and `ModelLoader=direct`, call the model's own
`Init_Ext`/`CreateFeature` without asking the driver at all. The risk here is therefore the version itself, plus the
user switching route (UseProxy off, ModelLoader, Retry) after any NGX call said `FAIL_OutOfDate`. Hence two refusals,
both in `retoque/dlssnr/DlssNr_DriverGate.h`:

1. **Version.** `ApplyDriverGate` (`DlssNr_Session.cpp`), called once from the GPU worker (`getGpuInfo`, dllmain) after
   `IdentifyGpu` has read `NvAPI_SYS_GetDriverAndBranchVersion` for the primary GPU. NVIDIA, native Windows, known
   version below 61088: the block is published and `DlssNrEnabled.set_volatile_value(false)`. The ini is never
   written (`value_for_config` returns the user's value while volatile) and `Config::Reload` keeps it (it only fills
   unset values). Logged once as a warning; a supported, unknown or Wine version is logged as information.
2. **Latch.** Any NGX result of `FAIL_OutOfDate` (`0xBAD0000C`) seen on the NR route sets a process-wide latch: the
   direct runtime's `Init_Ext` and create, the forwarder's results read back after a failed build, and the proxy's
   `CreateFeature(18)`. While latched, or while version-blocked, the D3D12 build, `DirectRuntime::Create` and
   `Proxy::Run` refuse before touching the model. A direct-runtime create that answers `FAIL_OutOfDate` with a handle
   returns `nullptr`; the handle is abandoned, never evaluated or released.

## Not gated

- **Wine/Proton.** dxvk-nvapi reports Linux driver numbers, which do not compare with Windows'. Gating on the NGX
  loader's build instead (`DlssNr_NgxInfo::DriverFromLoaderVersion`) was considered and not done: on 2026-09-28 Linux
  615.71.09 shipped wine loader 32.0.16.1691, which that function reads as "616.91", and nothing establishes that the
  wine loader's Windows-style number tracks the feature support the 610.88 threshold stands for. Not clearly safe, so
  exempt. The latch still applies under Proton.
- **Unknown version** (NVAPI absent, failing, or fakenvapi): not blocked; the latch still applies.
- **Other vendors**: not applicable; nothing published, nothing logged.

## Off path, resources, reset

NR off or a non-NVIDIA GPU: one `Publish` of a non-blocking verdict, no allocation, no hook. The gate owns no GPU or
OS resources. Nothing resets it: a driver update needs a game restart, which is also when the version is read again.

## Menu and controls (DEVELOPMENT.md section 2.5)

While version-blocked: an amber line above "Enable Neural Rendering" names the minimum, the recommendation and the
detected version and says the settings are unchanged; the checkbox is shown disabled; the session guard's "Enable
anyway" and the "Off. The model stays loaded" line are hidden; `SessionGuardEnableAnyway` refuses; the NR hotkey is
consumed and shows the same message as a warning toast. While latched (any platform): the status line reads "Off for
this session: <reason>" and "Retry" is hidden, since a retry would be refused the same way.

## Known gaps

- Native Vulkan and the native D3D11 probe do not latch their NGX results; on native Windows the version gate covers
  them because it turns NR off as a whole.
- The version is published from the GPU worker thread. A model built before that thread finishes would not see the
  block; in practice the worker finishes long before the first upscaler evaluate, but this is not enforced.
- `DlssNrEnabled` is written from the worker thread like upstream's other startup `set_volatile_value` calls; the
  create sites read the atomic gate, not the setting.

## Acceptance evidence

Host: `python3 tests/run_all.py --only nr-driver-gate --strict` (decision header under ASan/UBSan, call-site guards,
fifteen killed mutations). Still needed: a Release build, then on native Windows (a) a driver at or above 610.88 logs
"meets Neural Rendering's minimum" and NR runs unchanged, (b) a driver below it (or a debugger-forced 57688) logs the
block, shows the disabled checkbox with its line, refuses the hotkey and leaves the ini unchanged after a save.
