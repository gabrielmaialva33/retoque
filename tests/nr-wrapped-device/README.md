# NR device under a wrapper layer

`python3 tests/nr-wrapped-device/run.py`

A wrapper loaded between the game and OptiScaler (ReShade through `[Plugins] LoadReShade`, a Streamline proxy)
gives the game command lists that report the wrapper's device, while resources stay unwrapped and report the device
underneath. Since 2026-10-30 every D3D12 NR route takes its device from the command list it records on
(`Dispatch`, `EvaluateAfterUpscale`, `EvaluateAtPresent`, `ScopedPreUpscale`) or the queue it submits to
(`RunPresentPass`), as jlrouzies' `1c63318d` / `6724d677` do; see
[dx12-robustness.md](../../docs/dlssnr/design/dx12-robustness.md).

The suite compiles `retoque/dlssnr/DlssNr_DeviceMatch.h` and the production `DeviceChildBelongsTo` pair from
`DlssNr_Dx12_Resources.cpp` against fakes that model a wrapper (its own interface, same adapter LUID) under
ASan/UBSan: lists stay held to the exact device, a resource may report the device on the same adapter, another adapter
or a zero LUID is refused, the wrapper case is logged once, and every device reference is returned. Source guards then
read each route's device source and resource check, with seven mutants (each route reverted, the list rule relaxed,
the resource rule made strict, the overload undeclared) that must fail.

The routes themselves run against host fakes in `nr-before-upscale` (both stages under a wrapper, a foreign resource
behind a wrapper's list) and `nr-guide-copy-contract` (guide clones through a wrapper; the Dispatch prefix accepting a
wrapper's list for its own pass and refusing it for another pass).

Not proved here: that ReShade's or Streamline's proxies behave as modelled, that NGX accepts the wrapper's device, or
anything on a GPU. That needs a game with `LoadReShade` on native Windows and under Proton.
