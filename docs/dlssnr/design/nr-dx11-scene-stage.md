# Scene-stage NR with the native D3D11 upscaler retained

Date: 2026-11-04. **Phase 3 POST-SR implementation, opt-in; transport measured, game/model behavior not qualified.**
Source baseline: Retoque `e665dfee934219da1914a3b64e3f2cbd9408eba3`.
Donor reviewed: NeuRotic `beae4eeaab0032b665f7a52684db3aa7345bef69`, GPL-3.0.
Paths and line anchors below refer to those snapshots. Donor material is design evidence only;
no donor code, shader, bytecode, binary or model is imported.

## Current decision (Phase 3)

The approved product path keeps native D3D11 DLSS and runs NR on its successful evaluation's
**actual Output, before the wrapper's later effects and before the game's later effects**. It is
startup opt-in only. Pre-SR remains out of scope. The native backend calls the fork-owned scene
entry; the generic wrapper and original-handle forwarding paths are unchanged. Frame generation
is refused as `FrameGenerationUnsupported`: its extra queues are outside the alias proof below.

The installed runtime supports the measured transport with distinct queues. **A generic Proton
route without queue-identity evidence is not safely achievable under this contract.** An aliased
runtime is refused as `Dx11SceneStage: SharedVkQueue`; a runtime without the required interop
interfaces is refused as `QueueIdentityUnavailable`. There is no CPU-wait or keyed-mutex fallback.
The positive fixture establishes transport on this installation, not NR/model quality or arbitrary
game/native-Windows correctness. The earlier design and Phase 2 evidence below are retained as
history; this section and the Phase 3 implementation section supersede their proposed slot count,
pre-SR option and product-blocking decision.

## Current gap and donor evidence

| Evidence | Consequence |
|---|---|
| `retoque/dlssnr/DlssNr_Dx11Upscaler.h:48-61` | With NR enabled and `Dx11Upscaler` unset, the DLSS default becomes `DLSS_on12`. An explicit native `dlss` still wins. |
| `OptiScaler/upscalers/dlss/DLSSFeature_Dx11.cpp:73-103` | Native DLSS calls the original D3D11 NGX evaluator; this is the backend to retain. It does not execute the NR compositor. |
| `OptiScaler/upscalers/IFeature_Dx11.cpp:254-314` | The wrapper redirects Output to the first downstream pass, calls `EvaluateInternal`, then runs its effects and menu. The scene seam must precede those effects and target the actual redirected output. |
| `OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp:789-791,865-886` | Render-geometry scope already identifies the evaluate. `NoteDx11Evaluate` happens after the wrapper, too late for the requested placement. Raw original-handle forwarding at `:792-799` also bypasses the wrapper. |
| `retoque/dlssnr/DlssNr_Dx11Guides.cpp:465-588,592-645` | Existing native-guide transport owns snapshots, skips busy slots, and captures depth/motion for Present. It does not return an edited scene color to the native upscaler. |
| `OptiScaler/with_dx12/dx11_with_dx12.cpp:212-249,251-295` | The bridge enqueues a producer GPU wait; its reverse direction explicitly chooses CPU synchronization for Linux D3D11 swapchain interop because of a shared-queue cycle. Reusing that helper violates this proposal's no-CPU-wait contract. |

The older [native-guides note](dx11-native-dlss-guides.md) mixes initial proposals with later
implementation/evidence. Its queue hazard at line 599 remains relevant; its original recommendation
to retain the bridge's CPU wait cannot satisfy this new task. This note does not change existing bridges.

NeuRotic's `OptiScaler/dlssnr/DlssNr_Dx11.cpp:547-620` copies pre-SR color, signals D3D11,
waits from D3D12, evaluates NR, then enqueues a D3D11 GPU wait and substitutes Color.
At `:636-647` it restores the parameter. At `:742-817` its post-SR path copies native output
to a shared texture, executes NR on D3D12, waits on D3D11 and copies back. Retirement is signaled
after the D3D11 consumer, including the pre-SR case at `:715-720`.
Its `OptiScaler/upscalers/IFeature_Dx11.cpp:263-285` scopes Prepare/actual backend/Complete
before later effects and distinguishes backend success from a backend-change return.
These are useful placement, restoration and ownership ideas. The GPU waits at `:611` and `:811`
are specifically **not** a safe Proton recipe. They are GPU waits, not evidence that the calling
CPU blocks; the hazard is the resulting queue dependency.

## Placement options

| Placement | Inputs and write destination | Benefit and cost | Recommendation |
|---|---|---|---|
| Post-SR scene seam | Snapshot the native evaluator's actual Output and matching guides; NR writes a private shared result; copy it back before downstream wrapper effects | Native DLSS inputs/history remain unchanged; output crosses both directions at display resolution; two interop edges | First candidate once transport safety is proven |
| Pre-SR input substitution | Snapshot render-resolution Color and guides; compose into a private D3D11-shareable input; lend it to exactly one native evaluate | Less color traffic at ordinary upscale ratios; changes the temporal upscaler's input; requires proven jitter/guide/region conventions and consumer retirement | Later independent qualification, reusing existing Stage semantics |
| Native D3D11 NR model | Run the model directly beside native DLSS | Avoids API sharing if it works; the existing probe is not a supported compositor | Separate investigation; no automatic probe in a game |

Neither scene seam guarantees HUD-free input: it precedes effects submitted **after** the evaluate,
but a game may already have drawn UI or effects into that input. Verify placement with captures.
Native means the real D3D11 DLSS evaluator remains selected; it does not mean the NR model uses D3D11.
FSR, XeSS, RR, Streamline-only paths and original-handle forwarding outside the wrapper are not
implicitly covered. Do not add hooks for them as part of an initial implementation.

For pre-SR, preserve all original typed/untyped Color slots and any affected region parameters
under an evaluate-scoped guard, including failure/exception/reentrant exits. Do not write the
game's Color. Initially refuse nonzero bases, padded allocations, arrays, MSAA, unsupported format
families and unvalidated jittered-MV conventions instead of adjusting them speculatively.
Render, output and motion extents, finite jitter/scales, exposure and reset flags belong to the
same evaluate snapshot. Reuse the reasoning in [before-upscale](nr-before-upscale.md); do not assume
that feeding jittered color without an explicit convention is equivalent to the donor's path.
Pre-SR failure leaves original Color selected and does not run post-SR as a second attempt.

For post-SR, capture after an actual successful native invocation and before the wrapper effect loop.
A generic wrapper `true`, a backend-change request or a skipped native call is insufficient.
Operate on the parameter Output at that point, which may be the wrapper's intermediate target,
not a cached final output. Preserve the native output until the private result is admitted for
copy-back; a recording success alone does not authorize a CPU-side completion claim.

## Sharing and synchronization contract

### What the APIs and Proton evidence establish

Windows exposes shared fences through
[ID3D11Device5::OpenSharedFence](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-opensharedfence).
An immediate-context [D3D11 Wait](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-wait)
orders later GPU work against a fence; it is the D3D11/D3D12 interop counterpart of a D3D12 queue wait.
This establishes API semantics, not the absence of a cycle in a translated application.

[DXVK's feature-support page](https://github.com/doitsujin/dxvk/wiki/Feature-support#shared-resources)
describes resource sharing as partial, dependent on Proton patches and driver format support.
Upstream [D3D11 immediate-context source](https://github.com/doitsujin/dxvk/blob/master/src/d3d11/d3d11_context_imm.cpp)
implements Signal/Wait by emitting fence operations through its command stream; a successful API
return is not GPU completion. These upstream pages were inspected on 2026-11-04; they are not an
identification or qualification of an installed Proton build.

Repository evidence in [the guide transport fixture](../../../tests/dx11-guides-transport/README.md)
records GE-Proton11-7/RTX 4090 sharing for specified depth and motion formats on 2026-10-15.
That harness waits on the CPU per frame, has no NGX/OptiScaler or overlapping slots, and does not
exercise scene-color round trips. Its depth-stencil visibility counterexample also shows why fence
completion alone cannot repair an unsupported layout/format transfer. It is not a pass for this route.

Keyed mutexes are not the solution to the return edge. Windows
[AcquireSync](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgikeyedmutex-acquiresync)
can block its caller; a zero timeout is only a try-acquire, and WAIT_TIMEOUT/WAIT_ABANDONED must
be distinguished explicitly from S_OK. D3D12 does not simply expose the D3D11 keyed-mutex surface;
the [resource-heaps specification](https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html)
describes its different interop surface. No D3D12 participation and end-to-end Proton keyed-mutex
protocol was established here. Do not enable that fallback, spin on AcquireSync, or treat possession
of a D3D11 mutex as proof that D3D12's use completed.

### Owned transport

Proposed files are `retoque/dlssnr/DlssNr_Dx11SceneStage.h/.cpp` for admission, evaluate scope,
receipts and transport; a pure policy header can hold the testable state machine. GPU orchestration
stays in `retoque/shaders/dlssnr/`, using existing compositor semantics through a narrow explicit
entrypoint. Only the D3D11 wrapper/native backend seam and lifecycle notifications need upstream-path
edits. Do not wrap `NoteDx11Evaluate` and mistake it for the scene boundary.

Use owned single-sample 2D shared textures, initially same-adapter NT-handle sharing, opened on the
exact participating devices. Probe resource creation/opening and actual format/bind/UAV support;
do not change the process-wide `DontUseNTShared` choice. No silent legacy-handle fallback.
Color, supported depth extraction and motion are copied into transport-owned allocations even
when the game's resources are already shared. Retain source references until their D3D11 copy
completes. Preserve the existing SRV-only depth-copy constraint; never blindly share a depth-stencil.
Validate color family, alpha, signed values and scene color encoding; HDR swapchain metadata alone
does not describe an intermediate scene texture. Use exact-format copies initially; unsupported
BGRA/UAV/color-conversion cases decline rather than alter the color contract.

Use a fixed four-slot maximum and a proposed 512 MiB transport allocation cap (textures and
auxiliaries counted using allocation sizes, overflow-checked), with no automatic growth or budget
eviction. Model/compositor allocations are accounted separately. Cap exhaustion skips NR; it never
waits for a slot or retires an in-flight slot. These are conservative initial policy choices,
not measured optimums.

Each slot owns a list, allocator, shared resources, handles, retained devices and monotonic values
on **three single-writer timelines**: D3D11 producer-ready `P`, D3D12 completion `N`, and D3D11
last-consumer retirement `C`. Never signal a completion value from the CPU to unblock a stalled
GPU. Shared images cross API ownership in COMMON with the requisite D3D12 transitions around
SRV/UAV/copy access, including UAV ordering and restoration. No simultaneous cross-API access.
Handles close under their ownership contract; the underlying resources remain retained until safe.

### Desired dependency graph, and the missing proof

The logical same-frame graph is:

```text
D3D11: scene/SR -> copy into owned inputs -> Signal P
D3D12: Wait P -> NR into owned result -> restore COMMON -> Signal N
D3D11: Wait N -> native SR reads private Color OR copy result to native Output -> Signal C
         next slot reuse is allowed only after all applicable P/N/C uses complete
```

It is acyclic in isolation. A sufficient integration proof must additionally establish that:

1. Producer signal P can be submitted and executed without needing anything after Wait N.
2. NR's queue can reach Signal N independently of later D3D11 work, including any existing
   bridge/FG/Present waits and NGX's internal submissions.
3. If translation aliases logical queues onto one physical queue, submission scheduling preserves
   that dependency graph; no earlier wait on that queue can require D3D11 work behind Wait N.
4. Command submission is not prevented by a CPU lock retained across a cross-API callback.

A counterexample is an earlier bridge queue wait for a future D3D11 Present signal, NR queued
behind it on an aliased queue, and D3D11 blocked at Wait N before reaching that Present signal.
The local bridge comment documents this class of hazard; this note has not reproduced an exact
scheduler trace. Creating another D3D12 queue or checking adapter identity does not rule it out.
Flush submits work; it is not proof that a producer signal completed or that all queues are independent.
Disabling FG narrows the graph but does not prove isolation from every other bridge/driver submission.

Therefore **no new unsatisfied queue Wait is allowed in the proposed Proton path**. A future
qualified transport may use the graph only after proving these four obligations for its exact
runtime/queue domain, with fail-closed admission outside that domain. Do not add a user override
for an unproved queue topology. Native Windows needs its own correctness validation too.

### Nonblocking, completion-first experiment

A transport-only fixture can avoid introducing either unsatisfied cross-API wait:

1. Reserve a fully retired slot. Submit D3D11 copies and Signal P, then Flush as needed.
2. Read P's completed value through the importing fence once. If not complete, end this delivery attempt. Retain the slot
   until P completes; do not write to the game's output and do not start NR for the expired attempt.
3. Only after P is observed complete, enqueue an already-satisfied D3D12 Wait P for the interop
   acquisition, submit the owned D3D12 list and Signal N. Timelines never rewind, and the checked
   object/value/generation must be exactly the one passed to Wait.
4. At the still-active evaluation seam, read imported N once. If incomplete, leave original Color/output
   selected and retire the result later. Never deliver it to another evaluate or after the seam returns.
5. Only if N completed and the evaluate identity still matches, enqueue an already-satisfied D3D11
   Wait N, the consumer and Signal C. Retaining the API acquisition avoids assuming that CPU polling
   alone replaces the runtime's cross-API visibility handling. Slot reuse still requires an observed C,
   via later nonblocking inspections. Exact transfer visibility remains a GPU-fixture obligation.

No polling loop, sleep, event wait, blocking Map, device idle or nonzero-timeout mutex acquire is
permitted for delivery. On a busy GPU, step 2 or 4 will ordinarily decline. Moving the same wait to
a worker does not solve the synchronous evaluate deadline; neither does a larger ring. No claim
of useful delivery rate or zero driver-internal stalls follows from avoiding explicit waits.
Measure delivery and skipped/expired attempts separately. This experiment can test transfer and
lifetime safety, but a run with zero delivered frames cannot qualify scene-stage NR.

## Admission, configuration and unchanged paths

Proposed new `[DlssNr] Dx11SceneStage=false` is a capability opt-in, **not a second placement knob**.
Existing `Stage=0` selects post-SR; `Stage=1` selects pre-SR. Initial entry requires all of:

- NR enabled, explicit `Dx11Upscaler=dlss`, effective `HookMethod=1`, and the new key enabled at startup.
- An intercepted native DLSS SR evaluate with a valid feature/geometry identity, immediate context,
  supported resources and stable device/queue generation. No deferred-context execution.
- A qualified synchronization backend; currently no Proton backend meets the product gate.
- No active FG/bridge-FG route, Ray Reconstruction, native Streamline NR, or competing NR route.
  Preserve existing driver/session/model availability gates. Initially reject proxy execution,
  debug/compare/frame-hold and other unqualified combinations with an explicit reason.

`Dx11Upscaler` remains authoritative. The new key alone does not change the existing automatic
`dlss_12` choice. A conflicting backend or HookMethod reports an inactive scene request and keeps
the explicitly selected existing route. Once the native scene request is admitted as route owner,
all its declined frames remain native-only; Present NR and native-guide capture for Present must
stand down for that owner, including cold initialization and failures. Do not use “model ran recently”
as the exclusivity test. Claim/release the owner outside renderer locks and scope it to the feature,
device generation and picture epoch. Runtime disable stops new admission but retains in-flight owners.
Restart/recreate is required to change startup route selection; never migrate the native backend live.

At the seam, immediately after the off check, call the existing `Pictures::Ask` with the original
Color identity under `RenderGeometry::Scope` (`DlssNr_PictureGate.h:45-74`). An Aside does no NR
allocation, snapshot, history update, transport or parameter mutation. Reuse the cached judgement
for both placements and carry its epoch with the slot. Consume it under the existing history lock;
late results from an older epoch are refused. Do not rejudge the temporary private Color as another
picture. Preserve all existing seams' unbound-evaluate behavior; this new experimental route
additionally refuses an unbound identity because it cannot prove which evaluate owns its return.
Feature recreation after enabling NR resolves that refusal; no global policy change is required.

When implemented, the key must round-trip through `OptiScaler/Config.h`, both read/save paths in
`OptiScaler/Config.cpp`, shipped `OptiScaler.ini`, and the root `Config.md` where appropriate.
Requested, effective, blocked and restart-required states are distinct. Until the route exists and
is qualified, add no live menu control or shipped key. Later expose a control only on supported
routes, with read-only status otherwise. This phase adds neither settings nor menu UI.

With the new key off, retain the exact existing default-upscaler decision, native evaluator,
guide transport, Present behavior, parameter contents and state restoration. No new fence,
device, queue, hook, model probe, texture, capability query or per-frame transport logging occurs.
With NR off, the new key cannot activate any of them. C++/HLSL constants and shaders are unchanged
by this proposal; future edits must retain the append-only/layout and bytecode rules.

## Lifetime and locks

Slot identity is `(feature generation, evaluate serial, picture epoch, device/queue generation,
slot generation, recording epoch)`. Keep CPU reservation, recorded, submitted, completed and delivered
as separate facts. Signal failure after any queued work quarantines its owner; an HRESULT failure
does not certify that nothing executed. Device-removal sentinel UINT64_MAX is failure, never completion.
Partial D3D11 copies also need P retirement even if NR preparation fails.

For pre-SR, parameter restoration immediately after the native call does not end the GPU's input
read: Signal C must follow that invocation's consumption on the same immediate context. Reject
backends with undocumented consumption on independent queues. For post-SR, C follows copy-back.
Without a consumer, reclaim only after all submitted producer/NR uses complete. Allocator Reset,
model-history reuse and shared-resource overwrite obey the same completion and recording identity
rules; a free transport slot alone does not prove compositor scratch/history is reusable.
Use [submission tracking](submission-tracking.md) for compositor usages and explicit owned-list
sealing/no-replay rules. Do not manufacture completion from ticks, a timeout or a successful Close.

Transport metadata uses a short reservation lock, released before context/NGX/renderer calls.
An exclusive reservation token keeps a slot from concurrent reuse while the lock is released;
the immediate-context operation remains on its calling thread. Refuse concurrent/reentrant use
of the same feature/context, rather than lending one Color scratch twice. Publish status after
releasing runtime locks. Do not hold bridge `SyncMutex`, a transport mutex or a tracker lock while
calling NGX or the compositor. Preserve renderer order: pre-upscale mutex before NR mutex, with
the driver-init gate innermost ([shared-state contract](dx12-internal.md)). Shutdown marks admission
closed, detaches metadata under its lock, then performs retirement without calling the renderer
under that lock. No new inverse lock edge is allowed.

Model ownership remains with the existing D3D12 owner; a conflicting device is a refusal, not a
replacement of global caches. Creating a second device just for this route is not a safe shortcut.
Stage, extent, format, exposure convention and picture changes invalidate history and create a new
generation only when capacity and lifetime permit. Initial product qualification uses fixed settings.
Resize, feature release, disable and teardown close admission first. Retire only objects whose full
use is proven complete; retain/quarantine unresolved generations under a bounded owner and latch
the route off when that bound is reached. On device loss abandon unresolved GPU objects according
to existing policy. No new game-thread drain, forced device reset or speculative freeing.

D3D11 conversions must restore everything they alter, including CS shader/class instances, views,
samplers, constants, CS/OM UAV bindings and counters, RT/DS state and viewport state if used.
Keep this guard inside the NR-active scope; do not broadly change the wrapper's off-path restoration.
The existing native-guide scoped guard is useful evidence, not a guarantee that a new shader touches
the same state.

## Validation and release gates

Phase 1 checks documentation only. The following tests are **planned**, not executed here.

1. **Host policy/contract suite**, proposed `tests/nr-dx11-scene-stage`: compile pure admission,
   ownership and dependency decisions; fake the real wrapper seam and NGX invocation. Assert exact
   call order, native-backend retention, actual-success gating, original/redirected targets, restoration
   on every exit, one placement per evaluate and no Present fallback. Include main/aside, same-sized
   features, unbound identity, recreation, stale epoch, concurrent/reentrant entry and route conflicts.
2. **Lifetime/scheduling adversaries:** delayed P/N/C independently; queue aliasing with a pre-existing
   bridge wait; unsubmitted/failed/replayed recording; failed producer/completion/consumer signals;
   failed Reset; UINT64_MAX; cancellation after partial copy; cap/slot exhaustion; device loss and
   disable during work. A fake scheduler must reject the cyclic graph, not “pass” it with a timeout.
   Required mutants remove C retirement, accept old evaluate identities, treat API return as completion,
   remove the main-picture gate, replace actual backend success with wrapper success, allow an
   unsatisfied Wait, enable a default-off allocation, or publish Color before its result is safe.
3. **Existing focused contracts:** `python3 tests/nr-dx11-route/run.py`,
   `python3 tests/nr-dx11-native-guides/run.py`, `python3 tests/nr-main-picture/run.py`,
   `python3 tests/nr-before-upscale/run.py`, and `python3 tests/nr-submission/run.py`.
   Verify new config round-trip, invalid values and complete absence of new work with either gate off.
   Register a future suite and regenerate the catalog only when implementation is authorized.
4. **Transport-only GPU fixture first**, no game/model: exact-frame numbered color/depth/motion
   patterns D3D11 -> D3D12 -> D3D11; pre-SR consumer markers and post-SR copy-back markers;
   at least four overlapping slots, partial failures and state sentinels. Do not add CPU readbacks
   between stages to make a failing production sequence pass. Read results only after test completion.
   Count attempted/submitted/completed/delivered/expired frames separately and verify every returned
   frame's identity. Force shared-queue contention and missing producer submissions in an isolated
   disposable fixture with an external watchdog, not in a running game.
5. **Exact runtime safety proof:** record Proton, DXVK, vkd3d-proton, driver and GPU identities;
   demonstrate queue mapping/submission ordering, including bridge interop, before enabling any
   unsatisfied GPU wait. Include a single-graphics-queue scenario. A timeout-free smoke alone does not
   prove the ordering argument. If runtime capabilities cannot express/enforce it, keep Proton blocked.
   Native Windows requires debug-layer validation of resources, fences, barriers and retirement too.
6. **Real model/game only after transport gates:** verify a title actually reaches native D3D11 NGX;
   use fixed settings, FG off, and a separate process per stage. Compare native NR-off, current bridge
   NR, current native-guides Present NR and the candidate. Capture the same frame before/after NR and
   after downstream effects, including alpha, HDR/signed values, UI and motion/cuts. Check later-effect
   ordering, native DLSS retention, temporal quality, model/total GPU time, CPU stalls, VRAM and delivery
   rate. Broaden to DRS/rebuild/lifecycle stress and FG only with separate proven coverage.

Record commit/dirty state, exact commands, binary/model hashes, fixture identities and failures/skips.
Do not redistribute models or raw personal captures. Host contracts and Release builds cannot prove
GPU correctness, useful delivery, HUD placement, performance or native Windows compatibility.
Do not qualify an always-declining implementation as successful NR. The lead's full host gate and
an authorized Release build follow implementation, not this design-only phase.

## Open gates before implementation

- A concrete game where retaining native D3D11 DLSS or earlier scene placement has demonstrated value.
- A qualified no-CPU-wait transport domain, or a decision to limit the next phase to a nonblocking
  transport fixture. No such domain is established by this note.
- Exact same-frame jitter/guide conventions for pre-SR and compatible resource/format contracts.
- Review of proposed route exclusivity, startup opt-in and bounds before any product settings appear.

Until those gates are met, retain the existing native-guides and `dlss_12` routes unchanged.


## Phase 2 transport-only result (2026-11-04)

**Answer: achievable in this isolated runtime configuration with shared fences and distinct native
queues.** D3D11 produced, D3D12 computed an inversion, and D3D11 consumed the matching current
frame without a CPU completion wait between those stages. All 24 frames matched byte for byte
in each of three cases (72 verified positive frames), including real DXGI Present and an early
Present queue wait. This is an observed transport result, not proof for every queue schedule,
NGX internals or a game's bridge. The product route remains blocked pending that broader proof.

The lead explicitly authorized unsatisfied GPU waits **inside the isolated fixture** for this
experiment, superseding the completion-first-only experiment proposal for Phase 2. No wait was
added to the runtime. [The registered fixture](../../../tests/dx11-scene-transport/README.md) uses
four shared slots, unique per-submission lists/allocators and single-writer P/N/C/B timelines.
It compiles a tiny fixture-owned compute shader and loads no NR model or OptiScaler DLL.

### Runtime identity and primitive observations

- GE-Proton11-12-x86_64, version receipt `1793687165 GE-Proton11-12`.
- DXVK `v3.1.1-174-gf86377680682448-GE-Proton(dirty)` (runtime log).
- vkd3d-proton `v3.1-1-g8293130eb-GE-Proton(dirty)+`, build `8293130ebe370b1+` (runtime log).
- NVIDIA RTX 4090, driver `615.78.08`. No tested queue/config overrides were inherited.
- `GetVulkanQueueInfo` succeeded: separately created NR and Present queues had **different VkQueue
  handles**, both family 0. This is the actual mapping of this fixture, not an inference from
  different COM pointers and not the mapping of a running game.

| Primitive | Observation |
|---|---|
| D3D11 shared-fence export / D3D12 import | S_OK; P and C progressed to 24 in successful round trips |
| D3D12 shared-fence export / D3D11 import | S_OK; N progressed to 24; B progressed to 24 in Present cases |
| Shared RGBA8 texture, legacy handle | Creation and D3D12 opening S_OK; content round trip was tested with NT handles |
| Shared RGBA8 texture, `SHARED | SHARED_NTHANDLE` | Creation/opening S_OK; all positive frames exact |
| `SHARED_KEYEDMUTEX | SHARED_NTHANDLE` | Creation, D3D11 QI, AcquireSync(0,0), ReleaseSync(1), D3D12 resource opening S_OK |
| Wrong-key AcquireSync with zero timeout | WAIT_TIMEOUT (`0x00000102`), explicitly distinguished from S_OK |
| D3D12 resource QI for IDXGIKeyedMutex | E_NOINTERFACE (`0x80004002`); no keyed-mutex D3D12 synchronization route demonstrated |

An initial fixture revision incorrectly supplied `SHARED_NTHANDLE` without a sharing mode and
received E_INVALIDARG; it was corrected to the existing guide fixture's `SHARED | SHARED_NTHANDLE`.
That setup error is not evidence that valid NT sharing is unsupported. The keyed-mutex probe is
also deliberately narrower than “keyed mutex interop works”: its D3D12 acquire/release mechanism
is absent from this experiment, so it is not an alternative qualified route.

### Ordering outcomes

| Case | Delivered/verified | Present calls returned | Result |
|---|---:|---:|---|
| No Present | 24/24 | 0 | Exact current-frame inversion, P=N=C=24 |
| Normal Present on second queue | 24/24 | 24 | Exact inversion, P=N=C=B=24 |
| Present queue waits for future C before NR submission | 24/24 | 24 | Exact inversion on distinct native queues, P=N=C=B=24 |
| Forced same D3D12 queue, early Present wait | 0 verified | 2 | Known cycle; watchdog: P=1, N=C=B=0; third Present did not return |
| Copy instead of compute inversion (negative control) | 24 inspected, none correct | 0 | Pixel oracle rejected all 216,672 compared bytes |

**Not achievable for the deliberately aliased single-queue graph on this runtime.** Its earlier
Wait C blocks the NR work that must signal N; D3D11 is waiting for N before it can signal C.
The harness itself constructs this dependency cycle, and the fence observation agrees with it.
The 12-second watchdog exits only its process. An arbitrary timeout in an ordinary case would be
INCONCLUSIVE, not automatically attributed to a cycle or accepted as a passed test.

The synthetic alias is the same **logical D3D12 queue**, deliberately chosen as a deterministic
negative control. It does not prove that two separate logical queues mapped onto one native
VkQueue will behave identically; vkd3d's scheduling can differ. That topology remains untested here.
The positive early-wait case proves progress only on the distinct native queues actually observed.

There are no CPU completion checks in the submission loop. A final observer waits for C/B after
the entire batch and then maps snapshots with DO_NOT_WAIT; it cannot release any protocol fence.
The independent watchdog likewise never signals a fence. API calls can still block internally:
maximum measured CPU blocks in the final run were producer/NR-submit/consumer/Present respectively
5.518/0.408/0.004/0 ms without Present, 6.100/1.835/0.007/34.569 ms with normal Present, and
6.083/0.561/0.003/38.643 ms with early Present. These tiny, cold-start batches are **not** a
performance benchmark or proof that every API call is nonblocking. Returned Presents and D3D11
snapshots are not evidence of every frame reaching scanout.

### Receipt and remaining boundary

Source: baseline `e665dfee934219da1914a3b64e3f2cbd9408eba3` plus this uncommitted fixture/docs/registry
change. Built only the fixture with MSVC `/W4 /WX`, in its private compiler prefix.
Clang-format **22.1.8** was applied and checked. No changes under `OptiScaler/` or `retoque/`.

```bash
python3 tests/dx11-scene-transport/run.py --host-only
PROTON_DIR=/nonexistent/retoque-fixture-missing python3 tests/run_all.py --only dx11-scene-transport --log-dir tests/dx11-scene-transport/artifacts/skip-runner -v
python3 tests/run_all.py --only dx11-scene-transport --strict --log-dir tests/dx11-scene-transport/artifacts/runner -v
```

Results: 10 host evidence-classification checks passed; missing runtime preserved **SKIP** through
the aggregate runner (overall INCONCLUSIVE, not a GPU pass); final real-runtime suite **PASS**,
three positive round trips, primitive probe and both negative controls. The controls' raw STALL
and FAIL remain in their case evidence; only the expected detection is counted as passing.
Local ignored evidence: `tests/dx11-scene-transport/artifacts/result.json`, `runner/summary.json`
and per-case `trace.log`, `case.json`, `runtime.log`. Installed DLL hashes and private-prefix DLL
hashes matched; all are recorded in the aggregate. Executable SHA-256:
`6d51d4d13ce4d8a879902f9991a7976e01722bcefdd8b7e5efb231cd6d27d424`.

Still untested: native D3D11 DLSS/NR model internals; a game's real Present/FG bridge;
separate logical queues aliased onto one native queue; other GPUs/drivers/Proton builds; native
Windows/debug-layer correctness; HDR, depth/motion, jitter/subrects and dynamic-resolution lifetime;
scanout and visual quality. No general non-deadlock proof or product-path approval follows.

## Phase 3 implementation and validation (2026-11-04)

### Configuration and unchanged routes

`[DlssNr] Dx11SceneStage=false` is declared, read, saved and shipped with help text, also in
`Config.md`. The constructor snapshots it after the initial config load; reload/save changes the
next launch only. No menu toggle is added. Eligibility additionally requires master NR enabled,
an **explicit** `Dx11Upscaler=dlss`, `PresentBoundary::EffectiveMethod(...) == 1`, and `Stage=0`.
Unresolved Auto (effective 0) and Stage 1 do not enter. A matching nonzero `RenderGeometry` binding
is required before picture selection or transport.
The main-picture gate uses input **Color**, before the scene mutex, history updates, allocation,
queue inspection or NR. Output remains the actual transport/copy-back target, including a wrapper's
redirected output. Secondary pictures are untouched.

With the startup key off, no scene objects/queues are created. The original automatic `dlss_12`
choice and native-guide Present transport are unchanged. Transport/ownership failure latches for
the session without changing any routing/config key: native HookMethod 1 remains native without NR, and the existing
native-guide Present path remains available if its existing conditions later apply. The diagnostic
report and route status show named scene reasons instead of the old unconditional native-NoPass
warning once the opt-in seam has an answer. Build registration is explicit in the Visual Studio project.

### Queue admission and ordering

`DlssNr_Dx11SceneTransport.h` is shared by the product and GPU fixture. A private direct NR queue
uses the bridge's D3D12 device. `ID3D12DXVKInteropDevice::GetVulkanQueueInfo` obtains **actual VkQueue**
identities for NR and the bridge's consumer/Present queue. DXVK's read-only
`IDXGIVkInteropDevice::GetSubmissionQueue` obtains the native D3D11 consumer queue as well. Admission
requires all three identities, matching D3D12 device ownership, and NR unequal to both consumers.
The log records identities and families; different queue families are not required. Queries are
repeated before each submission; a bridge queue/device/context replacement refuses the session.
These interfaces are ABI prefixes of the installed translation layers, not Vulkan submission APIs.
Missing shared-fence interfaces, fence import/export or NT texture import has a named refusal.

Protocol, for each admitted frame:

1. Record the owned D3D12 list, including composition, before creating any GPU dependency.
2. D3D11 copies current output/depth/motion/optional exposure into private NT-shared textures,
   signals P and Flushes (submission, no completion wait).
3. NR queue GPU-Waits P, executes the closed list, then Signals N.
4. Only after N's signal has been submitted does D3D11 GPU-Wait N, copy the edited colour back
   into the same upscaler Output, and Signal C. Later D3D11 effects follow that copy in context order.

Every timeline has one writer. There is no CPU GPU wait, event wait, AcquireSync, polling loop,
or D3D12-to-D3D11 bridge CPU synchronization helper in the product transport. C is **observed once**
for admission; a busy frame is skipped unchanged, with a temporal reset on the next admitted frame.
No previous-frame answer is substituted. A declined model list is still submitted/sealed because
it may contain feature creation, but its colour is never copied back.

### Ownership, bounds and shutdown

The first implementation intentionally has **one live transport generation**, not the proposed
four-slot ring. It accepts single-mip, single-array, non-MSAA textures with supported typed formats,
full Output (no output subrect), and caps conservative two-view pixel storage at 512 MiB. The bound
excludes driver metadata and the renderer's separately bounded cache. Unsupported formats/subrects
refuse the session. Depth D32/R32 and D16/R16 copies use SRV-only shared allocations; stencil-packed
depth is not supported. All shared inputs return to COMMON before the D3D11 consumer. Owned colour,
guides and exposure have explicit renderer states independent of game barrier overrides.

Reuse, resize and replacement occur only after C **and** N complete with non-device-lost values.
No second generation is allocated while one is pending. A partial submission, Close failure,
failed signal/wait, or device loss permanently poisons ownership; the one retained generation is
not destroyed/replaced. Shutdown polls once; on success it Resets the owned list to end the exact
recording lifetime before destruction. Otherwise it retains the generation to process teardown.

The scene mutex uses try-lock on evaluate; lock order is scene -> renderer -> driver-init gate.
Shutdown visits scene before either renderer mutex. Scene never calls a renderer while holding
any bridge lock, and renderer code does not acquire scene. Mandatory submission tracking precedes
composition; the scene frame requests tracking even for one NR pass, refuses a prior untracked
renderer generation, and uses the existing Reset/Execute hooks and bounded renderer retirement.
The shared transport fixture substitutes a shader callback; it does not load that hook installer
or the NGX renderer. Those remain product/game validation obligations.

### Evidence and limits

The production-header fixture executes 24 spatially/frame-varying RGBA8 images, D3D12 in-place
inversion, D3D11 same-frame consumption, and an early queued Present dependency. All 24 snapshots
are compared byte-for-byte. Aliased NR/Present and missing queue evidence are rejected **before
submission**. The no-compute mutant completes 24 frames and fails the pixel oracle. Safe completed
transport retirement is exercised. Existing Phase 2 positive/forced-cycle cases remain enabled.
The host suite compiles the actual policy and checks integration contracts; 19 individually run
mutations must each fail (startup/default/native/hook/stage, device loss, both queue aliases,
missing queue evidence, memory cap, retirement, fence edges, declined copyback, picture gate,
tracking, config read/save and prior untracked lifetime).

No game or NVIDIA model was launched or deployed. HDR/FP16 colour, all guide formats, actual NR
composition, resize/device loss under load, wrapper/interposer variants, game FG queues, and native
Windows still require targeted runtime qualification. The serial completion admission can skip
frames and reduce NR coverage; its throughput/visual impact has not been measured. A transport
PASS must not be described as product rendering or image-quality PASS.

Measured Phase 3 fixture run: all ten cases PASS in 34.0 seconds (negative controls count as
PASS only when their required refusal/stall/pixel-failure oracle fires). Production:
NR/consumer VkQueue equality false, NR/Present equality false, all three families 0;
24/24 frames exact and completed retirement. The 15,598 busy-admission **poll attempts** in the
fixture are not game frames or a performance measurement. Fixture executable SHA-256:
`3f0b0634a56a30d0681c4931137cc51fe768f0f4a7f772335f35d0febac480c9`.
Runtime remains GE-Proton 11-12 / DXVK `v3.1.1-174-gf86377680682448-GE-Proton` /
vkd3d-proton `v3.1-1-g8293130eb-GE-Proton`, RTX 4090. Exact local evidence is in the ignored
`tests/dx11-scene-transport/artifacts/phase3-runner/summary.json` and per-case logs.

Product compilation: exactly one `./build-local.sh Release` using its shared compiler lock,
exit 0; no deployment. `OptiScaler.dll` SHA-256:
`4cea79bfed4b9bf751ee17ec8c338b5b5d4aea57e1b70f594dc0643da4f2b3f5`.
The forwarder SHA-256 is
`7dce9101534827790163f5f4920b2f2e9af2888e7ceac517b098a88cbcb49ab6`.
Three warnings were emitted (C4838 in untouched Streamline hooks, C4018 in untouched IFeature,
and LNK4098); compilation is not runtime qualification.

DXVK's read-only consumer ABI is documented in the primary
[dxgi_interfaces.h](https://github.com/doitsujin/dxvk/blob/master/src/dxgi/dxgi_interfaces.h)
(`IDXGIVkInteropDevice`, UUID ending `9323`); the vkd3d queue ABI prefix matches the declaration
already used in `OptiScaler/misc/IdentifyGpu.h`. Runtime queries, rather than the presence of these
declarations at compile time, decide admission.


## Review follow-up (2026-11-05)

The scene seam judges the original input Color, not the write destination. Different Colors sharing
an Output are different pictures; the same Color written to different Outputs remains a view of the
main picture. The actual Output is still copied into the transport and receives the result.

`FeatureIdentityUnavailableRecreate` refuses before `Pictures::Ask`, resource reads or transport
when the active RenderGeometry binding is absent, belongs to another parameter block or has key
zero. In particular, features created with NR off have no remembered creation identity: enabling
NR cannot admit them until recreation. This refusal is per evaluate, not a permanent session latch;
a recreated, bound feature can enter. The older seams' unbound Main/epoch-zero behavior is unchanged.

Once a transport owner is activated, an eligibility gap (including the master switch turning off),
an invalid/unbound evaluate, scene-lock contention, or an unsuccessful transport attempt leaves reset
debt. The next admitted main frame ORs that debt into the game's Reset even with stable dimensions,
device and picture epoch. Debt is atomically consumed **before** recording; success does not clear
a newer concurrent decline. Transport Busy/Passthrough restores it. A bound Aside returns before
any gap bookkeeping and cannot create or consume main-picture debt, including on a temporarily
ineligible stage. With NR disabled, RenderGeometry intentionally supplies no binding; an activated
owner conservatively remembers the gap without reading resources or judging pictures. Startup key
off and never-activated NR-off calls remain cold, without scene allocation or GPU work.

`FrameGenerationUnsupported` refuses before scene lock/device preparation/transport whenever the
known FG signals indicate an active or selected FG path: D3D12/Vulkan providers (including paused
but active providers), Streamline's mode/request/interpolation count, FSR input activity, DLSSG game
input enable, or a selected FG input/output. ForceXeLL alone is not an FG path. The selected-route
checks intentionally over-refuse paused/idle FG configurations: an extra queue can still exist.
The refusal recovers after those signals clear, with reset debt for an already activated owner.
This is a fail-closed restriction, not an expanded queue proof: the three checked queue identities
do **not** cover an FG queue. Unknown external injectors and game/model internals remain outside
qualification; there is no claim that a process-wide flag discovers arbitrary third-party queues.
No transport/fence/resource-lifetime changes are part of this follow-up.

Host regression coverage compiles the complete production scene entry with inert graphics and
renderer interfaces, alongside the real picture/geometry/policy headers. It checks both Color/Output
identity cases, off-at-creation then enable and recreate, absent/foreign/zero-key binding, stable-epoch
reset after disabled/stage/FG/busy gaps, Aside and cold paths, concurrent decline preservation, and
each FG signal. Nine additional mutants must **compile successfully and fail runtime assertions**;
compile failure is not a killed mutant. This complements the original 19 policy/source mutants.
These host tests do not exercise GPU queues, NGX, image quality or native Windows correctness.
