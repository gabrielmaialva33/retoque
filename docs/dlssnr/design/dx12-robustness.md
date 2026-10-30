# D3D12 NR robustness: wrapped devices, multi-view frames, multi-mip colour

Status: 2026-10-30, implemented on `fix/nr-dx12-device-views-mips-20261030`; host suites only. Nothing here has run
under MSVC, on a GPU, in a game or with ReShade. Three independent fixes, each adapted from a GPL-3.0 fork and
credited below.

## 1. The device is the command list's

**Report.** jlrouzies-fr/OptiScaler-DLSSNR-PreSR-Multipass, commit `1c63318d`: with `[Plugins] LoadReShade`,
access violations inside D3D12Core and hangs with NR before the upscaler. ReShade wraps the device, its queues and
command lists, and leaves resources unwrapped. A texture then reports the real device and the list the wrapper; NGX and
D3D12Core dereference the device behind the list being recorded, so a model made on the texture's device and
evaluated on the wrapper's list faulted. Their `6724d677` applies the same reasoning to a present queue that is
another wrapper of the evaluate's device.

**Ours before.** Every route took its device from a resource: `Dispatch` and its timing device from the output,
`EvaluateAfterUpscale` from the output, `ScopedPreUpscale` and `EvaluateAtPresent` from the colour, `RunPresentPass`
from the backbuffer. Unlike the fork, every route then held the list and all resources to that device pointer
(2026-10-27, [dx12-resource-ownership.md](dx12-resource-ownership.md)), so a wrapper could not reach NGX here: the
frame was refused ("inputs belong to another device") and NR never ran under `LoadReShade`. The fault was not
reproducible in this tree; the feature was simply off.

**Now.** Each route takes its device from the list it records on (`Dispatch`, `EvaluateAfterUpscale`,
`EvaluateAtPresent`, `ScopedPreUpscale`) or, for the present pass, which records on a list of its own, from the queue
it submits to (`RunPresentPass`). The pass, NGX's feature, the scratch and the cache owner (`NrDeviceOwner`) are that
device. `DeviceChildBelongsTo` keeps its strict rule for lists and queues; a new `ID3D12Resource*` overload accepts a
resource whose device is that one or another interface on the same adapter LUID (`retoque/dlssnr/DlssNr_DeviceMatch.h`).
D3D12 keeps one device per adapter in a process, so a same-LUID interface is a wrapper, not an independent device; a
zero LUID is refused. The first such acceptance is logged once. Without a wrapper every pointer matches as before, so
the default path takes the same decisions; only frames previously refused for a wrapper now run.

Left strict, and therefore still declining under a wrapper (fail-safe, not validated):
`DlssNr_PresentHdr_Dx12::ValidResources` compares the HDR textures' devices to the pass's by pointer, so HDR present
conversion declines; present guide capture (`CaptureGuides`, a pinned body) takes its device from the game's depth and
holds the list to it, so it captures nothing under a wrapper (and its stamps would not match the queue's device anyway),
and the present pass runs on zero or synthesized motion. Changing either is a separate step with its own suites
(`nr-hdr-d3d12`, `nr-present-guide-identity`, `nr-dx12-internal`'s body contract).

**Tests.** `tests/nr-wrapped-device` (the rule and the production overload pair, route source guards, seven mutants);
`tests/nr-before-upscale` (both stages under a wrapper; a foreign resource behind a wrapper's list);
`tests/nr-guide-copy-contract` (guide clones through a wrapper; the Dispatch prefix). The fakes model ReShade's
documented behaviour; they do not prove it.

## 2. Several views in one colour allocation: stand down

**Report.** janblade/OptiScaler-F5-DLSSNR-Multipass, commit `1105908f`: Assetto Corsa EVO on a triple-monitor rig calls
the upscaler once per screen plus once for the combined picture, all in one colour allocation. NR rebuilt the model on
every call, never evaluated, and parked resources piled up.

**Taken.** Only the detector and its default: two or more distinct active rectangles in one colour allocation (same
size and format) within one observed present, on two consecutive presents, and every evaluate stands down until 300
judged presents pass with one view each (`retoque/dlssnr/DlssNr_MultiView.h`). The second consecutive frame is an
addition: a present counter that stopped advancing would otherwise turn an ordinary resolution change into "several
views". Epoch 0 (no present observed) is not judged. Both D3D12 seams ask: `ScopedPreUpscale` after it owns the frame
and before any scratch loan, standing down rather than declining (a decline would hand the evaluate to the
after-upscale pass, which would run over every screen), and `EvaluateAfterUpscale` after the present-route deferral.
Each stand-down is counted (the coverage reason, the session total logged at the first and every 3600th) and owes the
history a reset; the layout's arrival is a warning, its departure an info line. There is no setting, so no control
and no configuration key.

**Not taken.** The view pick (`MultiViewTarget`), and the per-view models that followed in `358bc0cb`: one model per
screen is a different design with its own memory and lifetime questions, and nothing has measured it.

**Parked resources.** In this tree, untracked (single-pass) retirement ticks only where `Dispatch` reaches an
evaluated frame (and in present guide capture); the tracked chain also ticks through `RetiredCapacity` at every
recording admission. Rebuilds that park a generation are gated by the 500 ms extent gates (`g_postExtent`,
`g_preExtent`, `g_chainExtent`), so per-call alternation never settles, never rebuilds and parks nothing; the
stand-down returns before `Dispatch` and parks nothing either. A pile-up remains possible in principle on the untracked
path: an extent that changes on exactly the call after each settled rebuild would park one generation per settle
window with no evaluated call to tick it. No log has shown it. Ticking on calls that do not evaluate would change the
untracked evaluation-delay heuristic, which counts recorded evaluations as its only evidence of GPU progress, so the
lifetime policy is unchanged and this stays documented.

**Tests.** `tests/nr-multi-view` (ten detector cases on synthetic sub-rectangles, seven rule mutants including "never
stands down", seam source guards); `tests/nr-before-upscale` (two screens plus the combined picture through the
production seams in both stages; one view with moving resolution never stands down).

## 3. Multi-mip colour before the upscaler: decline

**Idea.** hhkbble/OptiScaler commit `08fa575e` admits typed multi-mip colour before the upscaler by transitioning
only mip 0.

**Ours.** `ScopedPreUpscale`'s colour contract did not look at `MipLevels`, and its read scope moves the colour with
`D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES` from the one state the game declared (`Barrier`,
`DlssNr_Dx12_Resources.cpp`). A game that builds a mip chain can leave other mips elsewhere, which makes that barrier
invalid. A subresource-0 transition would be correct only if every later use read mip 0 alone; the encode's views, NGX's
own views of the colour and the captures are not proved to, so Stage 1 now declines colour with more than one mip
level before any device query, barrier or allocation, with its own reason, counted as a fallback. The after-upscale
pass serves the evaluate, as it does for every other declined contract.

**Tests.** `tests/nr-before-upscale` (2 and 11 mips: declined, nothing touched, the after-upscale pass dispatched).

## What needs a machine

- MSVC Release build of the branch (none of this has been compiled by MSVC).
- `LoadReShade` in a D3D12 DLSS title on native Windows and under Proton, Stage 0 and Stage 1: the log line naming the
  wrapper, NR running, no D3D12 debug-layer errors, no device removal; whether ReShade wraps resources in that build.
- Assetto Corsa EVO (or any triple-screen or split-screen title): the warning, the counted stand-downs, the release
  after leaving the layout, and a single-view title showing none of them.
- A title with multi-mip colour at Stage 1: the decline reason and the after-upscale pass running.
