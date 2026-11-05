# Native Vulkan NR before the game's upscaler

Date: 2026-11-04. Status: **stage 2b implemented locally under the revised evaluate-window contract;
host validation and build results are recorded in the handoff report. GPU/game qualification is pending.**
Source inspected: Retoque `e665dfee934219da1914a3b64e3f2cbd9408eba3`.
This is item 2 of the NeuRotic comparison. It proposes an opt-in native Vulkan NGX input route;
it does not change D3D12, the Vulkan-on-D3D12 bridge, the present host, or the Stage default.

## Implemented boundary (2026-11-04)

Stage 0/auto preserves the original post-upscale implementation, whose complete function body is hash-guarded.
Stage 1 with NR enabled now constructs `ScopedPreUpscaleVk` around each native real SR call. It admits only proven
resources/identity/recordings, prepares an owned render-size Color, and restores the exact original pointer before
`UpscaleEnd` or return. Selected Stage 1 never invokes post-upscale NR, including refusals and partial failures.
The recording implementation is separate from the preserved Stage-0 body; shared allocation, composition, guide,
owner and recording primitives remain the existing fork implementations. No shader bytecode changed.

Implemented files:

- `DlssNr_VkPreUpscale.h`: standard-library admission/outcome rules and exact-ticket single-outstanding policy.
  They are consumed by the scope and host-tested with compiling mutants.
- `DlssNr_VkPreObserve.h`: thread-safe passive records, bounded by object and binding counts. Image/view facts,
  device/feature/image/view incarnations, command-pool family, allocation level, Begin flags/ticket, explicit image
  barriers, rendering state, conventional graphics/compute pipelines, ordered descriptor bind calls (including
  copied dynamic offsets) and ordered copied push-constant payloads. It owns no Vulkan objects.
- `DlssNr_VkPreRuntime.cpp/.h`: independent, per-address passive detours installed only when a device is created
  with NR enabled and Stage exactly 1, from either existing device-creation path. Each hook forwards once and
  returns the driver's result unchanged; failed result-bearing calls do not publish success facts. Same-API
  export-to-device nesting is observed once. Core/promoted alias targets share the same trampoline bank.
- `DlssNr_VkPreNgx.h`: successful NGX creation records type, explicit device and fresh incarnation; release erases
  the feature conservatively even if downstream release later fails. Device destruction erases all its records.
  Features created outside the observation window remain unknown.

Opt-in means no new detours, registry lookup, allocation or observation on a device created with NR off or Stage 0;
only the cheap configuration gates at existing creation seams execute. If hooks were installed while Stage 1 was
on, their forwarding stubs remain installed. The first observed call while off/Stage 0 latches an observation gap:
no new facts are captured, and later enabling does not requalify old evidence. Restart is required for a fresh
observation window. This avoids locks/allocations on the ordinary disabled command path; feature release/device
cleanup may still erase previously captured metadata. No observer enables/reinstalls itself from evaluate.

### What the observations do and do not establish

`Runtime::Status(device)` reports attached, missing and failed targets; its `complete` field refers **only to this
adapter's enumerated lifecycle/layout/rendering targets**. `Uninterrupted()` reports absence of an observed disabled interval or caught
observation failure. Neither is an admission certificate. Consumers must check both, current incarnations and the
specific facts; the scope refuses missing evidence. Missing late-created layer/proc-address targets, target-bank exhaustion
and unobserved device creation cannot be treated as coverage. Binding targets are consistently diagnostic: neither
missing targets nor failed binding attachments gate admission, including descriptor-buffer EXT commands.
For each promoted lifecycle/layout/rendering pair, at least one device-proc spelling must exist; an absent KHR
spelling is not a gap when its core spelling exists (and conversely). Every non-null proc-address/export target
is still attached, including distinct aliases; a failed attachment remains a mandatory coverage failure.
A loader export alone cannot establish the applicability of an otherwise missing device command. Core Vulkan 1.3
with no KHR aliases or descriptor-buffer extension is covered by the runtime-adapter fake. Device creation also records formatless-storage and format-flags2 enablement from the effective
create-info/API version; physical-device support alone is not enablement. Real Detours/driver coverage still needs validation.

Layout evidence is local to the exact command recording and exact observed view range. It is not propagated from
other buffers or inferred from an NGX wrapper. Conflicting old layouts, ownership-transfer barriers, invalid ranges,
image-handle reuse, active/unknown rendering state, traditional render-pass implicit transitions, secondary execution
and event waits with unmodeled dependencies refuse or invalidate evidence. `VK_REMAINING_*` ranges are not expanded;
subresource unions are not reconstructed. Image/view `pNext`, flags and swizzles are preserved as a `simple=false`
qualification boundary, not silently accepted as the plain view contract. The scope checks compute support for the observed pool family and requires concurrent sharing that names this
family on every admitted game image. **Exclusive ownership is not proved and exclusive game images are refused.**
It requires simple single-sample 2D creation facts, exact wrapper/view/range agreement, no aliases enabled by image
flags, and the required usage/layout. The initial route requires physical Vulkan 1.3 support as well as observed device enablement. Per-format
storage writes without a declared shader format are checked for
scratch and the floating-point composition format. This is deliberately narrower than all valid DLSS inputs.

Every object map, including per-command image layouts, uses `(device, handle)` keys. Device records use
`(device, device)`. Views have their own creation incarnation as well as the referenced image's incarnation;
recreating a view of the same image/range cannot authenticate an old view. Device-specific resets and destruction
cannot erase another device's same-numbered objects. APIs with only a dispatchable command-buffer handle resolve
its observed device only when unique; ambiguous synthetic collisions refuse observation. NGX release supplies no
device, so creation also records the game's handle pointer as an opaque source identity; release erases only that
pointer plus numeric feature ID. The pointer is never dereferenced by the observer. Unknown creations stay unknown.

The observer copies conventional pipeline/descriptor/push bindings, not descriptor contents or a replay program.
Push descriptors/templates, descriptor-buffer bindings, shader objects and the newer parameter-structure binding
variants invalidate binding evidence. Secondary command execution invalidates both binding and layout evidence.
Bindings are diagnostics only. Graphics/dynamic/vertex state is not captured, and nothing replays application
bindings. The revised isolation argument is the complete NGX evaluate window, below.

There is no independent observer recording counter. `VkPre::Ticket` aliases `VkRecording::Ticket`;
`CommandFact::ticket` is optional. The Begin detour snapshots the lifetime registry before forwarding and after a
successful native Begin. It associates only a new, nonzero ticket for the same device/buffer and matching observed
reuse flags. An unchanged, missing, unknown-reuse or mismatched ticket leaves the passive recording unqualified.
`Runtime::CurrentTicket` rechecks the captured ticket against the lifetime registry; it never repairs a missing
association. A lifecycle observer running too late, or changing the ticket again outside this detour, therefore
refuses admission rather than equating independently generated epochs. Nested same-API forwarding observes only
the outermost call. Real hook ordering/coverage still needs validation; fake ordering tests are not that evidence.
The new adapter only reads `VkRecording`: it does not allocate, begin, reset, retire or pin lifetime records.
Snapshot locks are acquired separately and released before acquiring the other registry's lock. Existing external
command-buffer synchronization requirements remain in force.

Device-create capability observation runs after the real create and outside the driver-init gate, while the
per-call `VulkanDeviceFeatureState` and extension storage still own the effective request. Observing at the end
of the outer hook would read destroyed feature-chain storage. `vk-create-contract` deep-copies this request in
its fake observer and compares it with what the fake driver saw; its late-observation mutant must fail under ASan.

### Revised step 3: the NGX evaluate window

The lead resolved the earlier missing compute-only proof by changing the contract: **no binding capture/replay is
required for isolation**. The official [NVIDIA DLSS Programming Guide, section 5.2.5](https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf)
(31 March 2026 edition, retrieved 2026-11-04) states, as quoted in the lead's approval:

> For DLSS titles that use DirectX 11, the NGX API preserves the state of the immediate D3D11 context. This is not
> the case with D3D12 command lists or Vulkan command buffers. For applications using DirectX 12 or Vulkan, the
> client application must manage the command list and command buffer state as needed.

Our isolation argument applies that contract to the **same evaluate call the game already makes**. All inserted NR
recording precedes the single real SR call, inside that window; SR establishes its own command state, and the game
manages the state it needs after evaluate. We make no claim that the opaque NR DLL is compute-only. Neither the
scope completion nor its destructor records GPU work or restores bindings over SR's final state. Source guards
check this placement; the extracted production scope is exercised with a fake prepare/SR and post-SR game rebind
plus sentinel. Those fakes verify the host contract, not real driver/model behavior.

Section 3.4 of the same guide separately requires sampled/read inputs and storage/write output, appropriate usage
flags, and return to those known resource states. **Binding-state freedom is not layout freedom.** Our stricter
Vulkan admission requires observed `SHADER_READ_ONLY_OPTIMAL` inputs and `GENERAL` output. Core Vulkan 1.3 is
required on the physical device as well as creation-time capability evidence and per-format sampled/storage/
write-without-format support. This follows the [Vulkan SPIR-V environment rules](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html)
for formatless storage; an instance version request alone is insufficient. Encode and guide matching
only sample foreign images; the NR DLL receives owned guide copies even at WorkingScale=1. Resolve writes only
owned scratch, then transitions it to `SHADER_READ_ONLY_OPTIMAL` before substitution. The original game's output
is touched only by the real SR. Active/unknown render pass or dynamic rendering always refuses.

The scope rechecks feature, recording and view incarnations before binding. Each accepted generation is tied to
the feature/device identity and command-pool family; a second live recording, repeated evaluate on the same ticket,
family change, or required size/filter/format/preset rebuild declines. Startup cold allocation has no explicit GPU
wait/submit; existing safe lifecycle cleanup owns reclamation. A transition back to Stage 0 waits for pre-stage
recording pins to retire before the old pass may reuse the shared model objects, including equal-size DLAA.
Under `g_vkMutex`, every pre model call also leaves `g_vkResetOnReturn` pending. The unchanged post body consumes
that latch under the same mutex, after its main-picture/recording gates and after the wrapper has proved pre
recording quiescence. The first post model call therefore receives reset=1 even when no dimension or generation
changes. A skipped call retains either the latch or the already-consumed `g_vk.reset`; only an actual model call
clears the latter. Later post calls resume reset=0. A post-only session never gets this new latch publication.

No submission receipts were added: the conservative history policy resets the model on **every admitted pre-stage
evaluate**. It never infers temporal continuity from CPU order or retirement alone. This is a quality/performance
limitation to measure, not a continuity claim. The one-outstanding pin survives CPU scope exit, End and submission;
only the existing legal recording retirement releases it. Failed/partial NR and failed SR cannot trigger post NR.

The first fresh extent settles for 500 ms. Rebuilds are refused in the evaluate scope; restart/safe existing
lifecycle cleanup is needed for another generation. The first placement choice is process-latched with an atomic CAS: a cold Post caller already waiting for the
model mutex cannot be bypassed by a cold Pre caller or invalidated by cleanup. After Pre wins, Post borrows the
same CPU loan through its recording, closing the check-to-record race at equal-size Stage changes. After Post
wins, Stage 1 requires a process restart, even if a later device is recreated. Missing coverage, incomplete format enablement, or exclusive input images can make Stage 1 skip every
frame. The native menu reports requested placement, not successful execution. No live support or speedup is claimed.

The real validation target is **DOOM Eternal, native Vulkan DLSS on Proton**. No game run, GPU validation, real-model
sentinel dispatch, image quality or performance qualification has been performed.

## Gap and evidence

Source anchors below refer to the inspected revisions, not future line positions.

| Source | Observed behavior |
|---|---|
| `OptiScaler/inputs/NVNGX_DLSS_Vk.cpp:1019` | One `RenderGeometry::Scope` binds the input API, game handle and parameter block around the evaluate. |
| `OptiScaler/inputs/NVNGX_DLSS_Vk.cpp:1062`, `:1069` | The forwarded NGX call runs first; after-upscale NR runs only after success. |
| `OptiScaler/inputs/NVNGX_DLSS_Vk.cpp:1114`, `:1119`, `:1153` | OptiScaler's provider has `UpscaleStart`, real evaluate, `UpscaleEnd`, then native NR. The live backend's `_on12` types are excluded at `:1147`. |
| `retoque/dlssnr/DlssNrFeature_Vk.cpp:1552`, `:1661`, `:2720` | The public pass is void, reads `Output`, and resolves into that same image. Calling it earlier or replacing its `Output` parameter does not implement a safe pre-upscale pass. |
| `retoque/dlssnr/DlssNr_Menu.cpp:1446` | Native Vulkan explicitly reports after-upscale placement. The experimental combo below it belongs to D3D12 and its bridges. |
| `retoque/shaders/dlssnr/DlssNr_Dx12.cpp:5816`, `:6228`, `:6248` | D3D12's scope prepares a separate color, substitutes only after a successful write, and restores parameters on scope exit. Its per-evaluate decline is distinct from stand-down. |
| `retoque/dlssnr/DlssNrFeature_Vk.cpp:1576`, `:1619` | Existing Vulkan asks the main-picture rule before its state lock and consumes the picture epoch under `g_vkMutex`. |
| `retoque/dlssnr/DlssNr_VkRecording.h:40`, `:48` | Tickets identify device/buffer/recording epoch. Pins survive End, submission and completion; current reuse metadata distinguishes serial from simultaneous execution, not one-time execution. |
| `OptiScaler/hooks/CommandBuffer_StateTracker.h:21`, `:182` | `LOW_PRECISION_TRACKING` is defined; image layouts and render-pass state are compiled out. Existing recording admission is not proof of a readable input layout or a legal compute insertion point. |

The [D3D12 measurement](stage-before-upscale-measured.md) is one game/session at DLSS Quality, with RR off.
Its timing and subjective quality result are not a Vulkan prediction. Lower-resolution NR is the experiment's
motivation, not a promised speedup or an argument for changing the default.

### Donor, ideas only

NeuRotic `beae4eeaab0032b665f7a52684db3aa7345bef69`, GPL-3.0:

- `OptiScaler/inputs/NVNGX_DLSS_Vk.cpp:1259` authenticates selected native SR, excluding RR and bridges;
  `:1268` associates the call with recording and feature generations; `:1304` evaluates before SR;
  `:1370` scopes parameter substitution around the real provider, including restoration on exceptions.
- `OptiScaler/dlssnr/DlssNrFeature_Vk.cpp:2749` is its pre-SR entry; `:2763` checks command state and input;
  `:2787` rejects an active render pass; `:2788` resolves the input layout; the low-precision branch refuses;
  `:2798` onward restores captured bindings. This is more than moving an after-SR call.

These are source-level ideas, not independent proof of correctness, performance or game coverage. No donor code,
shaders or bytecode is to be copied. Keep our [NR rules](DEVELOPMENT.md), [main-picture rule](main-picture.md),
[recording lifetime](vulkan-recording-lifetime.md) and [guide geometry](reduced-scale-guides-vk.md).

## Placement and responsibilities

Use the existing `[DlssNr] Stage=1` with `Enabled=true`. Stage 0/auto remains the existing path. Introduce a small
fork-owned `ScopedPreUpscaleVk` API in `retoque/dlssnr/DlssNrFeature_Vk.h`, implemented with the Vulkan route.
Pure admission/outcome rules belong in a new `retoque/dlssnr/DlssNr_VkPreUpscale.h`, tested without a GPU.
Only narrow scope construction/completion and the post decision belong in the upstream input file.

The intended command order, all in the **game's original recording `InCmdList`**, is:

1. Authenticate the current SR evaluate and its original resources. Judge its picture. Complete all fallible
   CPU validation and reserve/pin the entire NR budget before the first NR command, including feature creation.
2. Read the original render-resolution color, prepare color/guides, run NR, and resolve to an owned render-size
   scratch image. Never write the game's input or its upscale output during this preparation.
3. Make scratch writes visible to the real SR reader and substitute `Color`. Do not replay bindings: the
   application owns command state across this complete NGX evaluate window.
4. Call the real upscaler exactly once on the same command buffer, with the same callback and original output.
5. Restore the original `Color` immediately after that call, before `UpscaleEnd`, post NR, or any fallback/return.
   Report the actual provider outcome to this scope. Propagate the original upscaler result as before.

For managed providers, keep `UpscaleStart` before the scope and `UpscaleEnd` after restoration. Its existing
inspection of the original inputs must remain true; an implementation test must verify this ordering. For the
forwarded call, construct the scope directly around `NVNGXProxy::VULKAN_EvaluateFeature`. Eligibility is independent
of handle-number thresholds: an unknown forwarded feature type must not be presumed to be SR.

Use the **live provider** to exclude all five `_on12` backends already listed at `:1148`, even during backend
changes/fallback. The bridge owns its D3D12 Stage choice. FG evaluates never enter the scope. Track successful
feature creation's type, device and incarnation in fork-owned metadata, and erase it on release/device destruction;
do not infer RR solely from mutable parameters, the requested backend, or `State::currentFeature`.
`RenderGeometry::Creation` currently records sizes, not type/device/incarnation (`DlssNr_RenderGeometry.h:25`).
A feature created while NR was off may therefore remain unqualified until recreated; do not guess its identity.

### Local outcomes, not a global fallback flag

The scope snapshots Stage and routing policy once. Settings changing inside the provider call cannot select both
stages. Its result must distinguish the following; a bool cannot express the boundary:

| Outcome | Upstream parameters | After-upscale NR for this evaluate |
|---|---|---|
| NR off / Stage 0 | Original, no new preparation or observation | Existing behavior unchanged |
| Other picture / view stand-down / bridge / FG / game's own NR | Original | No new native pass; bridge and existing route-specific behavior remain theirs |
| Stage 1 selected, rejected before recording | Original; named reason for main picture | No automatic Stage-0 fallback in this initial Vulkan experiment |
| Work recorded but preparation failed | Original; retain every pin | Never a second NR attempt; invalidate this main picture's continuity |
| Scratch complete and bound | Edited `Color` only during real SR | Suppressed, including when SR subsequently fails |

The explicit no-fallback policy on Stage-1 refusal avoids alternating one shared model between render and display
sizes, and avoids escalating a resource/admission failure into another recording attempt. This differs from
D3D12's eligible after-stage fallback and must be stated in the Vulkan status/help. RR is excluded from the first
pre-stage implementation; it is not silently rerouted to post NR. Stage 0 retains its existing RR behavior.
Unknown Stage values keep the existing effective Stage-0 convention. No hidden retry or duplicate evaluate.

## Main picture, frame identity and history

After the NR-off/Stage-0 fast exit and null validation, call `Pictures::Ask` from `DlssNr_PictureGate.h` with the
**original game's Color image allocation**, before status, counters, settling, owner claims, resource admission,
history or exposure changes. Its area remains the declared **display** area, never the smaller render scratch.
The existing `RenderGeometry::Scope` spans pre, provider and post; its cached judgment is shared with the post
seam and bridges. Never judge twice or judge the substituted scratch. Unbound calls retain the rule's existing
main/epoch-0 answer, but that answer alone does not qualify unknown Stage-1 feature metadata.

An Aside evaluate performs no NR work and changes no main-picture reset flag, meter, status or recording state.
ViewOfMain still needs a multi-view stand-down before work: initially refuse non-full-picture/multi-view inputs
instead of selecting a view. Consume `Pictures::EvaluateEpoch(params)` through `g_vkPictureEpoch` **under
`g_vkMutex`**, as the existing route does. Reject an older epoch; a newer one resets history, held exposure,
readback validity and exposure plausibility. Do not replace this with a global boolean reset set by a seam.

Separate identities explicitly:

- picture epoch: which picture owns the history;
- API/feature incarnation + device incarnation: which live feature supplied this evaluate;
- buffer + recording epoch + per-evaluate sequence: which recording produced this scratch;
- original color image/view/subresource, render/display regions, guide regions and representation: what it means.

A sequence number identifies a CPU evaluate; it is **not** a rendered frame ID, a submit order, or GPU completion.
Release/recreate of the same numeric handle must invalidate old scope metadata without changing the main-picture
rule's existing single-feature successor semantics. Recheck the captured feature and recording before binding.

Read geometry, game reset, create flags, motion scales, jitter and exposure from the original evaluate, before the
provider can mutate it. Reject partial/stale geometry rather than treating allocation padding as rendered pixels.
The initial supported color contract is a single-sample, full, zero-origin 2D color view matching the current render
extent, with a supported format and explicit image/view facts. Padded/nonzero-base color, layered/stereo views and
unknown view facts decline. Dynamic render resolution can qualify after a 500 ms stable extent, subject to old
recording retirement; preserve the declared display extent for high-resolution motion-vector scaling.

Stage-1 model extent is `render extent * WorkingScale`; keep render, display, working and allocation sizes distinct.
Reuse the guide matching rules, with low-resolution motion referenced to render size and high-resolution motion
to display size (`DlssNrFeature_Vk.cpp:2241`). Do not simply replace every `width` in the after-stage routine with
render width. Larger guides must be matched even at Stage-1 WorkingScale=1 when required by this input contract;
the after-stage's existing below-native exception must stay unchanged. Keep jitter/exposure/color interpretation
consistent with the original input and leave all SR temporal parameters untouched in the initial scope.

Reset the model for a game reset, picture change, stage/representation/geometry change, skipped main-picture gap,
failed SR after binding, abandoned recording, or unproved temporal continuity. Old picture work arriving late
must not consume a newer picture's reset. Restoring parameters is not a history commit, and recording a dispatch
does not establish that it executed.

## Ownership and recording lifetime

Reuse the existing input-route model owner, currently named `Who::AfterUpscale`, for both placements; do not create
a second owner that can race the present host. Include placement, format and source geometry in the generation
contract even when render and output sizes happen to match (DLAA). Stage switches must quiesce the old generation;
do not run the old stage while waiting for the selected stage. Owner hand-back is still blocked by recording pins.

The initial experiment deliberately admits **one outstanding Stage-1 model recording** at a time, with one eligible
evaluate per recording. This includes its scratch, intermediate images and model history. Other main-picture
evaluates pass through unchanged until that recording retires. This conservative bound avoids a new ring whose
shared history or scratch is overwritten by a second queue, a later evaluate in the same buffer, or reverse submit
order. It can skip many frames in a game that retains command buffers; that is a real usability/performance limit,
to measure before considering a broader, separately proved scheduler.

Require an observed primary, one-time recording without simultaneous-use. Current `Reuse::Serial` is insufficient:
extend observation of Begin flags/level for the new admission only, leaving existing ticket identity and Stage-0
semantics intact. A reusable, unknown, secondary or simultaneous-use recording is refused before any GPU work.
Hold the generation and scratch lease through real SR and through the registry until successful reset/rebegin,
free or pool destruction. **Do not release on the scope destructor, End, submit, fence completion, a fixed number
of frames, or `vkDeviceWaitIdle`.** One-time admission does not weaken the existing retirement mechanism.

For future temporal continuity (the initial implementation resets every admitted frame), attach a scalar receipt to the exact Stage-1 ticket. Successful submit observation and
subsequent legal recording retirement may establish a submitted predecessor; reset without submission is a
discard. Cover `QueueSubmit` and `QueueSubmit2`, failed submission and handle reuse. No receipt means reset on the
next admitted main frame, never an invented continuation. Skips between admits likewise reset: one frame's motion
cannot reproject a history several real frames old. The receipt is not a new license to free anything. All receipt
and observation work must remain absent on NR-off/Stage-0 paths. This is proposed instrumentation, not a capability
of the current registry.

Reserve the complete conservative composition budget (currently four dispatch slots), optional scaler/guide
resources and generation/scratch leases before feature-create work; failures before emission release only things
proved unrecorded. Any failure after emission retains them through the recording. Scratch wrapper storage also
survives the CPU loan; no stack wrapper is installed into the parameter block. No budget growth on repeated
refusal. The [existing lifetime design](vulkan-recording-lifetime.md) remains authoritative for retirement.

Resize/filter/format/stage changes stop new pins while old recordings remain. Reclaim only with both recording
quiescence and the existing live-device safety proof. A lost or unproved device abandons objects without Vulkan
destruction; feature release alone does not retire a command buffer. Keep all scratch/images/descriptors/model
parameters in the owned generation, including resources referenced by a failed prepare or failed SR call.

## Layout, queue and binding contract

Do not assume that pre-SR `Color` is GENERAL because post-SR `Output` is GENERAL (`DlssNrFeature_Vk.cpp:2451`).
The layout is not carried by `NVSDK_NGX_Resource_VK`. Before Stage 1 can be enabled, a fork-owned observation layer
must establish the selected image/view's device, usage, format, range, incoming layout and compatible command-pool
family, plus a recording point outside a render pass/dynamic-rendering instance. Observe the successful API paths
actually used (including proc-address and synchronization2 variants); missing or conflicting evidence declines.
Late enable cannot reconstruct unobserved creation facts. Do not globally remove `LOW_PRECISION_TRACKING`, use
whole-image guesses for subresources, or let NR-off pay for an unrelated full-state tracker.

Start with the proven sampled-input convention `SHADER_READ_ONLY_OPTIMAL` and formats that the composition shader
can sample and the owned scratch can store/sample. Reject other input layouts until separately supported. Inputs,
depth/motion and exposure remain read-only in their proven layouts and owned by the game; the pass performs no
ownership transfer for them. The initial implementation refuses exclusive images because it cannot prove their current owner. Concurrent-sharing
metadata must include the compute-capable recording family. Unknown/cross-family transfer requirements decline, not a guessed acquire barrier.

| Resource step | Required dependency/exit state |
|---|---|
| Original Color and guides → encode/match | Earlier producer writes visible to compute sampled reads, in each proven incoming layout; no foreign discard transition |
| Owned proxy / matched guides → model | Compute writes visible to model reads, using the existing Vulkan NGX convention and validated model barriers |
| Model answer → resolve | Model writes visible to resolve reads; preserve existing encode/resolve color transform and passthrough gates |
| Resolve → scratch → real SR | Scratch STORAGE write in GENERAL, explicit write-to-read dependency and transition to the authenticated SR sampled-input layout |
| End of scope | Original parameter pointer restored; original foreign layouts unchanged; scratch remains leased in its recorded exit layout |

Fully overwritten owned images start from UNDEFINED in each fresh recording, as `BeginRouteRecording` already
requires; CPU layout bookkeeping from a discarded recording is never evidence that its transitions executed.
No new queue, submit, wait semaphore, CPU fence wait or `QueueWaitIdle` is inserted around SR. Existing rebuild
paths contain `DeviceWaitIdle` (`DlssNrFeature_Vk.cpp:1979`, `:2066`): Stage 1 must not blindly inherit a blocking
rebuild while inside the game's evaluate. Reuse a ready generation or decline while a separately safe lifecycle
boundary performs required teardown. A retirement path may reclaim resources proven unused by every recording;
otherwise defer or abandon. No background thread waits on a queue whose progress requires the caller to return.
The queue hazards in [native D3D11 guides](dx11-native-dlss-guides.md) apply as a constraint, not as permission to
copy that bridge's synchronization protocol.

Bindings are intentionally not restored, under the revised section-5.2.5 contract above. Resource layouts,
recording lifetime and parameter restoration remain mandatory. Source guards require the NR recording helper to
be called only from scope construction before real SR, never from completion/destruction. A host fake establishes
its own SR bindings and the simulated game rebinds before a post-SR sentinel; real validation remains pending.

Lock order stays: picture/geometry lookup finishes first; a nonblocking pre-scope loan guard prevents recursive
parameter/scratch borrowing; `g_vkMutex` protects preparation and history; short `g_ownerMutex` or recording
registry sections follow the route lock; `DriverInitGate` is innermost around model init/create. The pre-stage uses its new `TryScope`, which
refuses contention without entering the gate's condition-variable wait; existing blocking callers are unchanged. Registry deleters
run outside its mutex. Release route/owner/registry locks before calling the real provider/callback; keep only the
nonblocking loan guard and immutable scope/lease data. Completion restores only the parameter pointer; the reset-every-evaluate policy needs no post-SR history update. Never hold `g_ownerMutex` while acquiring `g_vkMutex`, and never acquire the loan guard
while holding the route lock. A nested call declines rather than waiting on itself.

## Parameter transaction and configuration

Save the exact original `Color` pointer and only bind after preparation and identity checks succeed. The candidate
wrapper describes the render-size scratch in the original input's supported format, with read-only
metadata for its use as an SR input. `Output`, dimensions, guide resources, subrects, jitter, scales, exposure,
callback and reset retain their original values. Never redirect `Output` to trick the old void API into recording
the new pass. Failure or exception restores the pointer through an idempotent RAII operation before any return;
provider writes to unrelated parameters must not be rolled back by copying the entire block.

There is no new config key. Existing round trip is `Config.h:556`, `Config.cpp:519` and `:1968`, shipped
`OptiScaler.ini:2265` (`Stage=auto`, effective 0). The INI/help descriptions and module
README distinguish requested native placement, without changing defaults or rewriting the user's stored value.

Expose the existing Stage combo on native Vulkan only when a live, authenticated SR route supports the experiment;
otherwise show a read-only effective-stage/refusal line. RR, bridge and present-owner states must not advertise a
native pre-stage that cannot run. HookMethod=2 does not itself prove the independent native Vulkan present host
owns the model; use actual routing/owner evidence. Keep existing Stage-0 routing and present policy unchanged.
Show requested stage separately from outcome: prepared-and-bound, skipped with original color, or waiting for old
recordings. Label this Vulkan experiment's no-post-fallback and one-outstanding-recording limits explicitly.

## Test plan and acceptance

Phase 1 ran only documentation checks. The passive implementation adds `tests/nr-vk-pre-upscale` for the rules, observations and
passive adapters, with compiling mutants. The following broader acceptance matrix remains; local completed gates and unrun boundaries are in the handoff:

1. **Host decisions and real scope with fake NGX/provider.** `tests/nr-vk-pre-upscale` is registered and the
   catalog regenerated; its extracted scope fixture implements this part of the plan. Exercise NR off/Stage 0, Stage 1, invalid Stage, forwarded unknown/type-qualified SR,
   RR, FG, every `_on12` type, backend switch and native present owner. Verify one real evaluate, exact callback
   and result, original inputs at UpscaleStart/End, scratch only during SR, and pointer restoration on every
   failure/exception. Mutate each condition and require its own expected rejection.
2. **Picture/temporal behavior.** Extend `nr-main-picture` for this fifth seam and run world/preview in both
   orders and sizes through the actual pre/provider/post scope. Aside never borrows, resets or changes status;
   test cached judgment, stale epochs arriving after new, handle reuse/release, views of main, and unbound calls.
   Verify resets on stage/geometry changes, skipped main frames, failed SR and discarded recordings; original
   jitter/reset/guide parameters remain untouched. Check guide scaling for low- and high-resolution vectors.
3. **Lifetime and fault injection.** Extend `nr-vk-recording` tests with observed primary/one-time flags. Submission receipts remain deferred; the
   current implementation resets history on every admission instead of assuming completed temporal continuity. Delay submission; reverse two buffers' attempted admissions; repeat a main evaluate in
   one recording; saturate slots; fail every allocation/pin/create/dispatch; keep executable recordings through
   resize/Stage switch/release. End/submit/completion must not release leases; reset/free/pool reset must release
   the right epoch. Unknown and reusable recordings refuse. Device loss abandons, reentrant deleters do not
   invert locks, and generation changes add no pins while waiting. A CPU mutex alone must fail the scratch test.
4. **Contract mutations.** Break original-color picture identity, epoch propagation, scratch lease, callback
   forwarding, restore-before-UpscaleEnd, Stage snapshot, post suppression, guide scale reference, barriers,
   layout evidence, feature incarnation and nonblocking admission. Each relevant behavioral/source guard must
   detect its mutation. Add source guards for new queue waits/submits and for late binding after failed resolve.
5. **Focused regressions.** Run `nr-before-upscale`, `nr-main-picture`, `nr-render-geometry`, `nr-vk-recording`,
   `nr-vk-guides`, `nr-vk-bridge-route`, `nr-vk-present-host`, `nr-vk-meter-readback`, `nr-invariants` and relevant
   menu tests, serially or in the brief's memory-capped scope. The lead owns the full host gate. Run docs checks.
6. **Real Vulkan fixture, then game.** A dedicated native pre-stage fixture must exercise the actual input scope,
   command ordering, fake patterned SR and readback before introducing real NGX. Prove core/synchronization
   validation enabled; sentinel input bytes and post-SR binding tests must pass. Include held recordings, resize,
   concurrent queues (refused), padded color (refused), DRS, device recreation and Stage 0/off byte comparisons.
   Then use the real model and native SR on the exact built DLL with captured driver/model/binary hashes.
   `guide-match-vk` and present-host tests do not qualify this new seam.
7. **Motion and cadence measurement.** Compare off, Stage 0 and Stage 1 in one native Vulkan game's repeatable
   moving scene, with identical model/WorkingScale/settings. Record admitted/skipped main evaluates, reasons,
   render/display/model sizes, GPU timestamps only with proven retirement, frame time and hitch distributions,
   disocclusion/shimmer, scene cuts and menu previews. Test quality ratios independently. The conservative
   admission may run too infrequently to be useful; report that result rather than hiding skipped frames in a
   model-time average. Repeat on native Windows before claiming native-Windows coverage.

Open for runtime review: actual lifecycle/layout hook coverage and ordering; broader exclusive-ownership
evidence; rebuild preparation outside the evaluate window; and whether the deliberately conservative recording bound is useful in the target game. If these
cannot be established, Stage 1 remains refused. Broader queue scheduling, subrect support, serial replay and
multiple outstanding model recordings require separate evidence, not a relaxed guard in this change.

Phase 1 changed documentation only. Stage 2a and 2b add observation, the live opt-in pre-SR scope and host
contracts. Release builds validate compilation/linking only; no shader regeneration, game run, GPU benchmark or
deployment has been performed.

## Refusals in the log and the observer's cost (2026-11-05)

The first game run ([DOOM Eternal](../../superpowers/reviews/2026-11-05-doom-eternal-vulkan-runs.md)) refused every
frame without a word: the scope's refusal paths returned silently, and because a selected Stage 1 never falls back,
that game got neither placement in-game, only the present host's cold NR. Each refusal path now sets a literal
reason (`_why`, or `VkPre::RefusalText` for the admission rule), and `ScopedPreUpscaleVk::Report`, called after
`Complete` on both evaluate paths, logs each reason and the first bound frame once per session. Nothing else in the
admission changed.

The same run measured the observer: about 39 presents/s with it active against about 120 once it went blind (NR
switched off and on), with the same NR pass cost. Every observed command takes the one mutex of `Observations`;
an engine that records on many threads waits on it. Stage 1 is not usable in such a game until the observer keeps
per-command-buffer state; whether a structural refusal should stand the observer down and return the frame to
Stage 0 is an open decision, not implemented.
