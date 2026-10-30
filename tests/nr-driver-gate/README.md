# NR minimum-driver gate

```bash
python3 tests/nr-driver-gate/run.py
```

About two seconds. Builds `cases.cpp` with g++ C++20, ASan/UBSan, `-Werror` and `-fno-sanitize-recover=all` against
`retoque/dlssnr/DlssNr_DriverGate.h` as shipped (header-only, no Windows headers), then checks the call sites by
source slice. Design: [driver-gate.md](../../docs/dlssnr/design/driver-gate.md).

Adapted from ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass `97f35314`, `521138df` and `dec66857` (GPL-3.0): the rule,
both driver numbers and the evidence (DLSS Unlocked #60) are theirs; the latch, the create sites and the tests are
Retoque's, because both loaders here call the model directly and the fork's NGX-first fallback does not exist.

## What it covers

- **The rule** (compile-time truth table): NVIDIA on native Windows below 61088 (610.88) is `TooOld`, 610.88 and
  616.64 are `Supported`, 0 is `Unknown`, and Wine/Proton or another vendor is `NotApplicable` whatever the number.
  Only `TooOld` blocks.
- **The latch**: only `0xBAD0000C` (`FAIL_OutOfDate`) latches; its neighbours, success and zero do not; it stays set
  after a later success; the loaders' `int` results convert to the same value. The version block wins the reason.
- **Text**: version formatting (`600.05`, `611.00`), the log/toast message and both fixed reasons naming the same
  numbers as the constants.
- **Concurrency**: publish and latch against readers on three threads, under the sanitizers.
- **Source guards**: `queryNvapi` reads `NvAPI_SYS_GetDriverAndBranchVersion` into `nvidiaDriverVersion`; `getGpuInfo`
  calls `ApplyDriverGate` with the vendor, the Wine flag and the version after the primary GPU is known; it resolves,
  publishes, and turns NR off only through `set_volatile_value(false)`; `DlssNrEnabled` is assigned only in the three
  known files; "Enable anyway" and the hotkey refuse while blocked; the menu disables the checkbox under its reason,
  hides "Enable anyway", "Retry" and the "Off." line where they would be dead; the menu line has a pt-BR entry;
  `DirectRuntime::Create` refuses before loading anything, latches `Init_Ext` and create results and never returns a
  handle from a `FAIL_OutOfDate` create; the proxy refuses before `CreateFeature(18)` and latches its answer; the
  D3D12 build refuses with the gate's reason before creating and latches a failed build's results.
- The header is listed in `OptiScaler.vcxproj(.filters)`.

Mutation-checked on 2026-10-30: fifteen single-point mutations (`<` to `<=`, the Wine exemption, the latch, each
create-site refusal and latch, the volatile setter, "Enable anyway", the hotkey, the disabled checkbox, the
proxy latch) each fail it.

## What it does not cover

It runs no NVAPI, NGX, model or GPU. Whether a real 576.88 driver reports 57688 through NVAPI, whether the model's own
`Init_Ext` or create ever answers `FAIL_OutOfDate`, and what the menu looks like are shown only by a native Windows run
of a Release build. The Vulkan routes are covered by the startup gate only; their NGX results are not latched.
