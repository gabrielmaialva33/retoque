# nr-drs-override

`python3 tests/nr-drs-override/run.py` slices `NvApiHooks::hkNvAPI_DRS_GetSetting` out of
`OptiScaler/nvapi/NvApiHooks.cpp`, with the fork headers that file includes (`DlssNr_DrsOverride.h`), and compiles it
against a fake driver, the vendored `NvApiDriverSettings.h` and stubs of `State`, `Config` and the log, under
ASan/UBSan (`g++`, C++20).

The NR setting ids and values in `cases.h` are copied from NVIDIA's R615 `NvApiDriverSettings.h` (NVIDIA/nvapi
`87dca625`, the headers dxvk-nvapi v0.9.3 builds with), not taken from production, so a wrong constant there fails.

Exercises:
- NR off: `NGX_DLSS_NR_OVERRIDE`, `..._RENDER_PRESET_SELECTION` and `NGX_DLSS_NR_SL_OVERRIDE` reach the reader byte
  for byte as the driver wrote them, and nothing about them is logged; switching NR off at runtime returns to that;
- NR on: the preset selection reads as off whatever the driver answered (A, latest, off); the two enable switches
  pass unchanged; each setting is logged at info, naming the id and the driver's value, at its first read and
  again only when its value changes;
- NR on, left alone: a failed read (status propagated), a non-DWORD value, a null output;
- the SR/RR preset neutralisation and the Streamline override behave the same with NR off and on; an unrelated id
  passes through.

Not covered: which component reads these settings on a given route (the model 310.8.x reads the preset selection
itself; see `docs/dlssnr/design/model-310-9-16.md`), whether that read goes through the hooked
`nvapi_QueryInterface`, and what a real driver or dxvk-nvapi does with the values. That needs a game run with the
override set.
