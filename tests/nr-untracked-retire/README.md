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

Live tracking opt-in regression (`migration_cases.cpp`) also compiles the production `TrackNrRecording`,
`WantsTrackedRecording`, private-list provenance and submission-completion helpers. It covers a pending CPU
recording, cancellation, delayed/removed GPU fence, an undrained generation, invalid recording lease, changing
back to one pass during migration, sticky tracking, failed tracker installation, individual/adaptive requests,
mandatory scene-stage refusal after unsupported history and a late submission on a second queue. Six
independent mutations remove detach, pending-list, device-loss, queue, individual-opt-in or drain guards; each
must be rejected by an executable scenario. Only the
resource-detach operation and collector are faked in that controller fixture; the collector itself is separately
compiled verbatim above. Its migration tests require the private submission fence even when another collector
entrypoint is used, prohibit count-only fallback and shutdown release, and retain on device loss.

Source guards inventory all `NrState` resource pointers and helper passes, verify no destruction at detach,
check private submit/cancel notification ordering, and require held composition references plus stale-generation
refusal. Runtime queue behavior, actual model rebuilds and native Windows/Proton compatibility remain unverified.

Live opt-in from the upscaler's output (chain-in-flight.md, 2026-11-20). The same production `TrackNrRecording`
runs these cases:

- the default one-pass path on that route: a thousand recordings make no tracker call (so no hook and no tracker
  lock), no adapter query, no park, no collector pass, no status write and no allocation (`operator new` is
  counted for the whole process);
- raising the count asks the adapter and the tracker once, detaches while tracking is still off, arms tracking in
  the same call and records nothing in it; the next call is an ordinary tracked recording;
- back to one pass and up again leaves nothing behind a second time, and neither does a forced loss of tracking;
- a second generation that does not fit, a runtime the tracker refuses, the driver proxy and a foreign recording
  lease each keep one pass running with their own status, and the first two are asked again only after 128 ticks;
- a bridge, before-upscale or private present recording in the history, an opt-in arriving on another route, and
  mandatory `Dx11SceneStage` keep the status they had;
- individual settings and adaptive cadence opt in the same way; a session that starts above one pass is tracked
  from its first recording.

Fourteen more mutations of that body must fail (20 with the six above): enrolling `Passes=1`, skipping the budget
or the tracker check, asking the adapter every frame, allowing an `Other` or a private history, another arrival
route, the mandatory stage or the proxy, retaining twice, not arming with the detach, omitting the detach,
recording on the detached generation, and dropping the one-pass fallback.

`cases.cpp` runs the production `LeaveUntrackedBehind` with the production collector, `RetiredCapacity` and
`PresentGuideRetirementCapacity`: what the untracked path parked leaves the retirement list, a present-guide clone
and a tracked entry stay, no count, fence, fallback or shutdown pass frees what left, and forty entries left behind
hold neither parking cap. Three mutations (the generation stays in the list, clones leave, tracked entries leave)
must fail. Source guards: the detach is `ParkUntrackedGeneration` without the fence gate and destroys nothing; the
left-behind list is written in one place and read only for the log line; the budget check asks and changes
nothing, and counts surfaces as `Dispatch` does; `AfterUpscaleRecording` is set once, in `EvaluateAfterUpscale`,
after the capture for the present; the before-upscale scope names no route; the one-pass branch is two
statements. The adapter query and the detach are counted fakes in the controller fixture. Not shown: the real
adapter budget, a real model rebuild, the pause, the picture, or anything on a GPU.
