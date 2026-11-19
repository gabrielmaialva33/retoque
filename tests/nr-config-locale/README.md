# NR and Config numeric locale verification

Run from the repository root:

```sh
python3 tests/nr-config-locale/run.py
```

Requires Python and `g++` with C++20 and ASan/UBSan. Temporary generated sources and
binaries are deleted automatically. No Wine, D3D12, Vulkan, NGX or GPU is required.

A host process that sets a decimal-comma C numeric locale (such as Qt processes under
`pt-BR` or `de-DE`) shares its C runtime locale with `OptiScaler.dll`. The suite slices
and compiles the production `isFloat`, `GetFloatValue`, `PackOverlayValue`,
`UnpackOverlayValues`, `Config::readString` and `Config::readFloat` definitions from
`OptiScaler/Config.cpp`, `DlssNr::PassConfig` from `retoque/dlssnr/DlssNr_PassSettings.h`,
`DlssNr::PresentCpu::Format` from `retoque/dlssnr/DlssNr_PresentCpuTiming.h`,
`LoadAnchors` and `SerializeAnchors` from `retoque/dlssnr/DlssNr_ExposureScan.cpp`, and
`ReportScalingRatios` from `retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp`.

It verifies:

- `GetFloatValue` writes fixed six-decimal dot strings (`0.500000`) and `"auto"` for
  absent or non-finite values regardless of `LC_NUMERIC`.
- `Config::readFloat` and `DlssNr::PassConfig::Float` parse dot floats in the classic
  locale, recover unambiguous single-comma floats (`0,500000`, `+1,25`, `-0,5`) when no
  dot is present, and reject thousands/multi-comma, mixed-separator, trailing-junk,
  overflowing, underflowing and non-finite inputs.
- Full `CSimpleIniA` save/reload round trips for `[DlssNr] WorkingScale`, master model
  settings, `[DlssNr.PassN]` overrides, FPS overlay export/unpack float values, and
  `[DlssNr] ScanAnchors` write dot decimals and canonicalize recovered comma entries.
- `DlssNr::PresentCpu::Format` and `ReportScalingRatios` emit dot decimals without
  commas.
- When a decimal-comma C locale (`pt_BR.UTF-8`, `de_DE.UTF-8`, or `fr_FR.UTF-8`) is
  installed on the host, the entire round-trip, recovery and log suite also runs with
  `LC_NUMERIC` live and `localeconv()->decimal_point` confirmed to be `","`; if none is
  installed on a minimal CI image, only the live-`setlocale` pass prints an explicit
  notice while the parser, serializer and source guards still run.

Passing proves host-side C++ numeric formatting, recovery and source guards; it does not
prove MSVC CRT behavior under a live Windows game without building the DLL.
