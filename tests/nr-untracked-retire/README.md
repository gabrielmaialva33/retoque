# Untracked D3D12 NR retirement fence gate

`python3 tests/nr-untracked-retire/run.py`

On the default one-pass D3D12 NR path (`g_tracked == false`), parked objects keep their 32-tick evaluation
countdown and also require a present-queue `ID3D12Fence` to have completed past the Present after parking
(`fence->GetCompletedValue() >= parkedSerial + 1`); see `retoque/dlssnr/DlssNr_UntrackedRetire.h` and
[chain-in-flight.md](../../docs/dlssnr/design/chain-in-flight.md).

The suite compiles `retoque/dlssnr/DlssNr_UntrackedRetire.h` and verbatim production slices of `NrRetired`,
`ParkNrFeature`, `ParkNrResource`, `ParkNrClone` and `TickNrRetired` from `DlssNr_Dx12_Internal.h` and
`DlssNr_Dx12_Lifetime.cpp` under ASan/UBSan:

- the 32-tick count gate still holds even if the fence completed early, and `StepCountdown` clamps at zero across
  arbitrarily long retentions;
- an expired count gate retains parked objects until `completedValue >= parkedSerial + 1`, querying the fence at
  most once per tick;
- `UINT64_MAX` (`DXGI_ERROR_DEVICE_REMOVED`), `deviceRemoved` without a usable fence, and `parkedSerial` values
  saturated near `UINT64_MAX` retain rather than wrapping or falling back to count-only destruction;
- live devices without a single present-queue fence (`NoSwapchainPresent`, `MultiplePresentQueues`,
  `SlotLimitReached`, `CreateFenceFailed`, `SignalFailed`) fall back to the exact 32-tick count-only rule and log
  once at `INFO`;
- `TickNrRetired(true)` keeps the shutdown path unchanged;
- source guards check that all 11 `ParkNr*` helpers stamp `presentSerial`, `NoteSwapchainPresented` exits before
  any COM work while NR is off or shutting down, all four D3D12 Present sites notify it once, and the header is
  listed in `OptiScaler.vcxproj` and `OptiScaler.vcxproj.filters`.

Not proved here: real DXGI/D3D12 swapchain queue ownership, Streamline or FG swapchain behavior, GPU execution
ordering, or native Windows / Proton compatibility.
