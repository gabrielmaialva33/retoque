# Same-frame pair capture on native Vulkan after-upscale NR

Status: **Phase 2 implemented, awaiting review and GPU qualification**, 2026-11-04, based on `e665dfee`.
The lead approved the Phase 1 proposal and supplied the D3D12 tracked-only patch, applied before this work.
The following design records that approved contract; the implementation receipt below identifies the concrete
modules, deviations and verification boundary. No native-driver/game qualification is implied.

## Intent and current gap

One explicit request saves the original and processed image of one native Vulkan after-upscale NR
invocation, plus provenance, through our existing PairCapture machinery. Linux/Proton comes first.
This is an upscaler-output diagnostic, **not a screenshot of scanout**, a comparison of separate frames,
or a second model evaluation. Native Vulkan present, Vulkan-to-D3D12 bridges and future pre-SR placement
are outside this first implementation. Bridges keep the D3D12 capture path.

Source anchors below are repository-relative and refer to `e665dfee`:

| Current fact | Evidence |
|---|---|
| Vulkan capture is expressly refused by the menu rule | `retoque/dlssnr/DlssNr_PairCapture.h:62,380-438`; `retoque/dlssnr/DlssNr_Menu.cpp:303-332` |
| A backend-neutral session already owns requests, recording, pending, writing and retention | `retoque/dlssnr/DlssNr_PairCapture.h:107-120,166-267,295-375` |
| The old untracked fallback must not reach this new backend | `retoque/dlssnr/DlssNr_PairCapture.h:40-48,181-208,260-262`; the lead is separately removing it |
| The current writer and payload are D3D12-specific, despite the generic session | `retoque/dlssnr/DlssNr_PairCapture.cpp:66-81,135-202,371-443,490-547` |
| Native NR enters under `g_vkMutex` with an observed recording ticket | `retoque/dlssnr/DlssNrFeature_Vk.cpp:1552-1590` |
| Encode reads the game's output in GENERAL; resolve writes it in GENERAL | `retoque/dlssnr/DlssNrFeature_Vk.cpp:2451-2468,2718-2725` |
| The retained original is always FP16, even for an FP32 game image | `retoque/dlssnr/DlssNrFeature_Vk.cpp:2011,2037-2044`; `retoque/shaders/dlssnr/precompile/dlssnr.hlsl:784-795` |
| NGX image metadata does not include image creation usage | `external/nvngx_dlss_sdk/nvsdk_ngx_vk.h:85-112` |
| Existing readback consumes only retired recordings; a sentinel rejects unexecuted work | `retoque/dlssnr/DlssNrFeature_Vk.cpp:665-715`; `retoque/dlssnr/DlssNr_VkMotionProbe.h:5-22,48-67` |
| Registry retirement is distinct from submission/fence completion | `retoque/dlssnr/DlssNr_VkRecording.h:19-52,104-126,176-204,275-295` |

Read alongside [comparison capture](comparison-capture.md), especially its implemented behavior in
section 10; [Vulkan recording lifetime](vulkan-recording-lifetime.md);
[NR invariants](DEVELOPMENT.md); and the [module index](../../../retoque/dlssnr/README.md).
The historical eight-frame capture rule is not evidence of completion.

## Prior art, as data

NeuRotic `beae4eea` publishes only after both GPU completion and recording release:
[OptiScaler/dlssnr/VulkanNrCapture.cpp:21-28](https://github.com/MagicalPrincessUnicorn/NeuRotic-an-OptiScaler-DLSSNR-fork/blob/beae4eea/OptiScaler/dlssnr/VulkanNrCapture.cpp#L21).
Its successful native-route capture selects private original/composed images:
[OptiScaler/dlssnr/DlssNrFeature_Vk.cpp:2598-2609](https://github.com/MagicalPrincessUnicorn/NeuRotic-an-OptiScaler-DLSSNR-fork/blob/beae4eea/OptiScaler/dlssnr/DlssNrFeature_Vk.cpp#L2598).
The useful idea is immutable recording identity plus safe readback publication. Its owner/submit tracker,
private composed-image topology and shader bytes are not our implementation.
Write our own code; retain this GPL-3.0 prior-art attribution and existing third-party notices.
No NVIDIA DLLs, donor binaries or user-owned LSFG assets enter this change.

## Approaches and proposed choice

1. **Two transfer copies after resolve:** smallest session change, but unsafe to assume the foreign output
   has `TRANSFER_SRC` usage. Our owned images have it (`DlssNrFeature_Vk.cpp:509`), the NGX output metadata
   does not prove it. Using `g_vk.keep` also silently reduces an FP32 original to FP16. Reject as the
   general solution. A later fast path needs actual creation facts and equivalent precision.
2. **Capture-only sampled snapshots before encode and after resolve — proposed.** Read the same float
   color view the existing encode already samples, in GENERAL, into private RGBA32F storage buffers.
   This avoids new foreign-image usage assumptions and preserves the values seen at both boundaries.
   It needs two small compute dispatches and a split recording interface in the shared session.
3. **Independent submit/fence tracking or a capture queue:** more hooks and ordering obligations, and
   still insufficient for legal replay until recording release. Reject for this scope.

Option 2 was approved by the lead and is implemented in Phase 2.
The Vulkan specification requires transfer-source usage for an image-to-buffer copy
([vkCmdCopyImageToBuffer](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImageToBuffer.html)).
We will therefore perform GPU sampled snapshots, not manufacture that missing usage proof.

## Proposed modules and shared contracts

- Keep `PairCapture::Session<Backend>` as the **only request/record/pending/write state machine**.
  Instantiate it for a new Vulkan backend in `retoque/dlssnr/DlssNr_PairCaptureVk.h/.cpp`.
  Put portable admission, size/format and ticket-result rules in a small fork-owned header.
- Extract API-neutral writer inputs and the existing PNG/PFM/sidecar, naming, rotation and failure cleanup
  into `DlssNr_PairCaptureWriter.h/.cpp`. Preserve D3D12 output semantics and exact-record tests.
  A writer job supplies stable CPU rows, provenance and a completion callback; no assumed DXGI adapter,
  D3D12 resource, global D3D12 session or Vulkan handle is required by the file-writing core.
- Add an independent sampled-to-buffer pass under `retoque/shaders/dlssnr/`, with our own
  `precompile/dlssnr_pair_capture.hlsl` and generated SPIR-V. Two immutable descriptor sets belong to each
  capture attempt. Do not change `dlssnr.hlsl`, its existing bytecode, cbuffer or eighteen-slot pool.
  No donor shader. Compile/verify the new source and header only after implementation approval.
- Reuse the public request/status API through a small backend router. A process-wide active-request
  token prevents simultaneous D3D12/Vulkan requests from racing the common writer/filenames/rotation.
  Freeze the selected backend at the request; never migrate a pending capture across a route switch.
  The menu's existing bridge classification matters; `State::api == Vulkan` alone is insufficient
  (`DlssNr_Menu.cpp:1314-1315`). No nested acquisition of D3D12 and Vulkan pass locks.

### Shared Session changes to coordinate with the lead

These are required semantics, not parallel edits to the lead's D3D12 patch:

1. `Track` failure ends the attempt **before any snapshot/copy** and frees only never-recorded allocations.
   There is no Vulkan `AssumedFrames` case. Depend on the lead's tracked-only common change.
2. Split the current monolithic `Record` into an owned attempt: claim/allocate/pin, record original,
   record processed/commit, or abort. Keep the D3D12 `Record` convenience wrapper using the same methods
   consecutively, so D3D12 does not acquire a different state machine.
3. The attempt owns its allocation handles before emitting commands. Its abort path distinguishes
   “nothing recorded” from “may have recorded”; the latter retains everything until retirement,
   without publishing. No throw after a first command may free a local RAII GPU bundle.
4. Poll needs a terminal **discarded/invalid** result in addition to pending/readable, so a recording
   reset without execution settles with a reason instead of becoming a perpetual pending capture.
   The implemented shared Session now uses `Ready` for both hand-off and release (the 2026-11-04 Ready
   follow-up), separate from “file saved.” `Completed` alone cannot protect a reader from replay.
5. Add a truthful completion label such as `vulkan-recording-retired+executed-marker`; do not reuse
   `submission-fence`. Reserve retention capacity before recording so `Retain` cannot allocate/throw
   after GPU commands or writer handoff. These invariants require shared host tests, not a second FSM.

## Recording sequence and image/layout contract

All backend recording/ownership changes occur under the existing `g_vkMutex`.

1. Only an explicit pending Vulkan request starts preparation. Validate the live native after-upscale
   route, settings, image view/range, formats, dimensions and bounded allocation plan. Unknown ticket,
   unknown reuse flags or `Reuse::Simultaneous` refuses capture only. Ordinary NR continues.
2. Prepare both host-visible buffers, descriptor/pipeline resources and immutable metadata; reserve the
   writer-job budget; claim the shared session attempt; pin one metadata retirement token into the
   **same** `VkRecording::Ticket` as the route's generation. Finish every fallible preparation before
   the first capture command. Initialize a per-attempt nonzero-cookie marker to zero while CPU-exclusive.
3. After ordinary NR admission/generation pinning, immediately before encode at the boundary around
   `DlssNrFeature_Vk.cpp:2451`, snapshot the game's original output into buffer A. Use integer texel loads,
   no filtering, exposure change, clamp, tone mapping or half-float intermediate.
4. Run the normal encode/model/resolve unchanged. Only after successful resolve at line 2725 snapshot
   the **same image view and region** into buffer B. These are not the soft-knee proxy and raw model answer.
   Revalidate CPU attempt identity, not mutable current settings, at commit.
5. Record a completion cookie after both snapshots, followed by a host-visibility barrier. Commit the
   attempt to Pending. On encode/model/resolve failure, abort publication and retain the partial attempt.
   Both snapshots, marker and normal NR commands are in the application's original command buffer.
   No private queue, submission, fence wait, device drain or frame-delay shortcut is introduced.

The source image remains **GENERAL throughout**, as the existing NGX route requires. A memory dependency
makes the upstream producer's writes visible to the first sampled read; an execution dependency orders
that read before NR's later write. After resolve, COMPUTE/SHADER_WRITE becomes visible to the capture's
COMPUTE/SHADER_READ. A final dependency orders the extra read before subsequent game writes. Use the
existing region's color subresource; no queue-family transfer or game-image layout change. Refuse views
outside the route's established single-sample, unprotected 2D color contract; no array/MSAA/depth/planar
reinterpretation. The supported subset requires one color mip/layer at base zero and matching extents.

The new pass writes float4 texels to buffers with `STORAGE_BUFFER | TRANSFER_DST` usage. The second bit
is for the small `vkCmdUpdateBuffer` marker, not permission to copy the foreign image. Buffer offsets,
lengths and row sizes use overflow-checked arithmetic and device `maxStorageBufferRange` limits.
Order shader writes before marker transfer, then make **both SHADER_WRITE and TRANSFER_WRITE** visible
at HOST/HOST_READ over the data and marker ranges. Barrier scopes must also order serial re-execution's
writes to these same buffers. All dispatches occur outside render passes, as the existing NR compute path.
Use core barriers; do not add a synchronization2 requirement to the user's device.
See [vkCmdPipelineBarrier](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPipelineBarrier.html).

Require `HOST_VISIBLE | HOST_COHERENT` memory in this first version; prefer HOST_CACHED when available.
If none fits, refuse capture without changing NR. Map on allocation, but do not read mapped bytes yet.
Coherence avoids explicit cache invalidation; it does **not** prove completion
([memory property definitions](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryPropertyFlagBits.html)).

## Completion, replay, ownership and teardown

### What proves readable

Retoque's registry does not provide NeuRotic's `GpuComplete` API. Use our stronger conservative boundary:

- A successful reset/re-begin/free/pool retirement drops the exact recording's lease. Legal Vulkan
  retirement cannot occur while that recording is pending and prevents future execution of it.
  The pin destructor publishes only an atomic retirement flag, using the pattern in
  `DlssNr_VkMeterReadback.h:28-48,116-125`; it calls no Vulkan API and takes no route lock.
- After an acquire of that flag, **and only on the still-valid device generation**, inspect the cookie.
  An exact match after the host barrier proves that the end of this capture executed. Zero/mismatch means
  discarded or incomplete: no pixel reads and no successful pair. Pixel brightness is never a sentinel.
- `End`, an elapsed number of frames, a signalled fence, a timestamp-query result, or `ForgetDevice`
  alone is insufficient. Administrative abandonment must mark the attempt unusable **before** dropping
  registry pins; otherwise device loss could masquerade as normal retirement.
- The registry is the only lasting strong owner of the retirement token after admission. The attempt
  retains the separate flag/identity state, not that token; otherwise its own ticket would prevent
  retirement forever. Capturing a generation pin in a writer job would similarly block route hand-back
  and is forbidden.

This relies on the same valid-application lifecycle contract as the existing meter/probe, not on tolerating
illegal application reset of pending work. See [vkResetCommandBuffer](https://docs.vulkan.org/refpages/latest/refpages/source/vkResetCommandBuffer.html).
Reject simultaneous/unknown reuse. Serial executable replays can overwrite this attempt's buffers, but
no CPU reads occur until the final retirement; the pair then belongs to the **last complete execution**
of that recording. Provenance must not call its CPU recording sequence a present/scanout frame number.
The [usage flag rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkCommandBufferUsageFlagBits.html)
permit simultaneous reuse only when declared; capture will not support that case.

### Ownership and writer lock order

The capture manager owns all Vulkan allocations, descriptors, mappings and device-generation identities
under `g_vkMutex`. Recording tokens own CPU retirement metadata only. The session and writer job hold
references to an attempt; releasing a reference never implicitly invokes Vulkan on an arbitrary thread.

`Hand` queues a writer job only after retirement and a valid marker. The worker releases its queue mutex,
allocates its CPU row vectors, then enters a narrow route helper under `g_vkMutex`: recheck live device/
attempt identity, copy mapped rows into CPU-owned bytes, and drop its readback claim. All unmap/destroy/free
operations are performed there or by the next owner poll, under `g_vkMutex`, after the last claim. Encoding,
means, model lookup, file I/O and completion reporting run **outside** that lock with CPU bytes only.
This deliberately pays a bounded memory-copy critical section on a requested capture; it is not a zero-
latency claim. Never hold `g_vkMutex` through file I/O or let a worker read a stale mapping during teardown.

Lock order: route lock -> registry pin call (whose deleters run after the registry unlock); route lock ->
writer enqueue mutex; worker dequeue/unlock -> route helper; session outcome lock is not held while taking
the route lock. Never join a worker under `g_vkMutex`, loader lock or device teardown. The worker never
waits for GPU progress. Existing `g_vkMutex -> g_ownerMutex` order remains intact. Route-owned helpers
provide access rather than exporting the mutex or duplicating it in the new backend.

`FreeRouteObjects`, `ShutdownVk(false)` and `AfterUpscaleDeviceGone` must explicitly notify the capture
manager (`DlssNrFeature_Vk.cpp:2769,2839,2977-3019`). Notify by the attempt's owning device/generation
even if `FreeRouteObjects` has already cleared `g_vk.device`; queued jobs must not escape teardown
notification merely because the renderer released the model first:

- Normal route release/resize: recording pins still gate the NR generation. Dispose unused capture
  resources only after retirement. A completed job may continue from CPU bytes after route release.
- Live actual device teardown after the existing drain: stop admission, invalidate jobs before
  `ForgetDevice`, serialize against the worker's short copy section, then release capture children before
  `vkDestroyDevice`. No new drain or worker join. A queued job without CPU bytes fails cleanly.
- Lost/unproven device: invalidate first, abandon handles and mapping addresses without `vkUnmapMemory`,
  `vkDestroy*`, GPU waits or pixel reads. A reused device handle has a new generation, never old authority.
- Failed recording, timeout, route cancellation or NR switched off never makes a GPU allocation reusable.
  Retain/quarantine it until genuine retirement or abandonment. The timeout can stop publication and lock
  out further requests; it cannot certify completion. An indefinitely executable recording can leave the
  request waiting/disabled. Explain this in status rather than forcing a reset of the game's command buffer.

Proposed bound: one active pair across backends; Vulkan owns at most four retained attempts and a **512 MiB
combined readback-plus-CPU-staging reservation**, using actual buffer allocation requirements plus planned
CPU rows. Reserve before GPU work, including quarantine and queued writer claims. Overflow, device limits
or budget exhaustion refuses capture. No eviction of in-flight resources and no unbounded retry allocation.
Four is a ceiling inherited from PairCapture, not an instruction to preallocate four pairs.

## Formats, provenance and opt-in behavior

The first implementation snapshots **sampled float values**, not exact source storage bits. FP16 expands
to FP32; FP32 avoids the existing FP16 keep image. GPU loads can canonicalize NaN payloads; do not claim
bit-preserved NaNs or byte-exact original image memory. Both snapshots use the same source-view decoding.

| Source view and established domain | Proposed output / admission |
|---|---|
| RGBA16F or RGBA32F, existing `linearHdr` decision true | PFM float32 RGB plus JSON, preserve negative/over-one values; no exposure adjustment or tone map |
| RGBA16F or RGBA32F, existing domain explicitly display-referred | PNG8 through existing float row conversion; record quantization |
| RGBA8/BGRA8 UNORM, display-referred | PNG8; sampled channels are already RGBA, so do not swizzle a second time; test all byte values |
| sRGB views | Refuse initially: sampled loads decode RGB, unlike raw byte capture; support needs an explicit shared re-encode contract |
| Packed 10/11-bit, PQ/HLG, unknown color domain, compressed/integer/other views | Refuse with format/domain reason; never guess HDR transfer from bit depth |
| Vulkan present HDR10/scRGB | Out of scope here; current present-host capture refusal stays |

Use the existing `gameSaysHdr && FormatCanHoldLinearHdr(...)` decision
(`DlssNrFeature_Vk.cpp:2250-2253`), not display settings. Alpha remains omitted by the image writer.
No HDR preview in this iteration. The readback row representation can reuse `ImageFile`'s float32 converter,
but provenance keeps actual **VkFormat names**, never casts VkFormat integers into DXGI format enums.

Freeze build, settings, extent, working scale, white point, route (`vulkan-after-upscale`/after-RR when known),
surface (`upscaler-output`), request ID, recording epoch/recording sequence and execution-marker cookie.
Record the source/view domain, float32 readback representation, completion evidence and replay limitation.
Record device/model facts without raw pointers, private paths, addresses or fabricated DXGI/LUID identities.
Resolve model/adapter facts outside the render critical section; snapshot device facts before teardown if
needed, and explicitly label unavailable facts. Keep UTC names, the shared 64-pair disk cap, atomic per-file
publication and rollback behavior. Add backend-specific fields without changing D3D12 records accidentally.

No persistent config key: one manual press is the opt-in. Retain proxy/Compare/debug refusals and the existing
ApplyModel semantics (an explicitly disabled model may produce an equal pair; label settings honestly).
Update the pure button table and DEVELOPMENT.md section 2.3 during implementation: NR off remains absent;
an eligible live native after-upscale route can request; native present/unknown routes remain disabled with
a reason; pending explains that completion **and recording release** are awaited. Use existing activity facts
for visibility, and validate detailed format/recording facts only on request. Request/status reads never poll
Vulkan or create the backend. Do not add a per-frame Vulkan trigger-file stat; D3D12 trigger behavior stays.

**Idle contract:** before first request, at most relaxed atomic gates at the Vulkan integration sites, no
allocation, clock, lock beyond the route's existing lock, descriptor write, GPU command, format/property
query, metadata construction, worker or file access. When used and later quiescent, the same no-work fast
path applies. Pending cleanup is real work and remains explicitly active even if the user later disables NR;
UI cannot declare it idle while resources are retained. Poll only from serialized route calls/teardown;
if NR is disabled before polling resumes, retain safely rather than adding an always-on worker/hook.

## Test and acceptance plan (future implementation)

Host tests must drive the real shared Session, Registry, token and format/size rules with fake Vulkan commands.
Add `tests/nr-vk-pair-capture`, register it in `tests/suites.toml`, regenerate the catalog only then. Extend
`nr-capture` for shared contracts and retain `nr-vk-recording` / `nr-vk-zero-motion` regression coverage.

| Test group | Required behavior; useful mutant |
|---|---|
| Shared state machine | One request, split recording/commit/abort, refusal and OOM at every preparation boundary, handoff failure, writer failure, timeout/quarantine. Track-false emits zero commands; mutant restoring the old copy-after-failed-Track must fail. D3D12 wrapper and Vulkan use the same states. |
| Lifetime | Delayed/unsubmitted work, End, signalled fence with replay still possible, serial replay, reset/free/pool reset, failed reset, stale/reused handles, second device, lost device, token outliving manager. Mutants reading after N polls or fence-only readiness must fail. |
| Execution marker | Valid black pair succeeds; retired never-executed and partial-before-only recordings produce no files. Marker from previous allocation/attempt cannot pass. Never inspect marker before retirement. Device abandonment before ForgetDevice must prevent every mapping read. |
| Locks and teardown | Registry callback publishes metadata only; worker/route/device-release interleavings; no queue mutex -> route lock inversion, no worker join under locks, no API call on abandoned device, queued/CPU-snapshotted job cases. |
| Exact boundaries | Fake command log proves original snapshot < encode < model < resolve < processed snapshot < marker/barrier, same ticket/view/region. Failure at each boundary retains earlier commands but suppresses publication. |
| Formats/bounds | All 256 UNORM codes/channel order; finite FP16 values, negative/over-one HDR, FP32 differences that FP16 would erase; zero/overflow dimensions, row bounds, maxStorageBufferRange, memory-type refusal, 512 MiB accounting including worker staging, unknown/PQ/sRGB refusals. No false bit-exact NaN claim. |
| Off and interface | Never requested and NR off emit zero capture API calls/allocations; route switches cannot steal requests; no Vulkan-request duplication on bridges; every menu/status row and no Vulkan polling from UI; D3D12 provenance/name golden tests unchanged. |
| Output | Decode actual PNG/PFM and compare to known rows; identical provenance for pair/sidecar, truthful completion/backend fields, write/rename failure cleanup and cap only after success; privacy checks. |

Planned focused commands: `python3 tests/nr-capture/run.py`, `python3 tests/nr-vk-recording/run.py`,
`python3 tests/nr-vk-zero-motion/run.py`, the new suite's `run.py`, and `python3 tools/check_docs.py`.
Run serially; the lead owns the full host tier. Production-body fakes must inspect actual command arguments,
not just search for barrier function names. Independent mutants should target publication before retirement,
missing host visibility, double release, aliasing a descriptor slot and source-domain mislabelling.

A Vulkan GPU fixture must exercise the **new production snapshot pass and lifecycle adapter** with analytic
frame IDs/pixel patterns, the real validation layer and synchronization validation. Cover FP16/FP32 HDR and
UNORM, serial replay before reset, discarded recordings, multiple in-flight recordings, resize, off/on and
normal/lost-device paths where safely reproducible. Decode files and match before/after from one execution;
fail on validation errors, missing execution evidence or stale output. No real NR DLL is needed for the
analytic fixture; a mock transform makes differences known. It cannot certify the model integration.

Then qualify the integrated native after-upscale route in a Linux/Proton game, including scene-linear HDR,
working-scale changes, request during loading/off, and native-versus-bridged routing. Measure capture-frame
GPU cost and worker-copy lock duration; neither “no waits” nor a host PASS implies no hitch. Native Windows
Vulkan is a separate validation target. Only these runs can establish shader correctness on the device,
actual hook/lifecycle coverage, same-execution game pixels and practical capture overhead.

## Phase 1 review history

The lead approved sampled snapshots, the shared split Session interface, and a bounded CPU copy under
`g_vkMutex`. The implementation applies the supplied tracked-only D3D12 patch before extending that Session.

## Phase 2 implementation receipt

- `DlssNr_PairCapture.h`: `BeginRecord` / `RecordOriginal` / `CommitRecord`, partial cancellation retained
  until safe release, discarded terminal outcome, allocation/hand-off exception cleanup. D3D12's `Record`
  still wraps the same state machine. `Track=false` emits no copies on either backend.
- `DlssNr_PairCaptureVk.cpp`: one lazy process-lifetime Session and manager, all resource operations under
  `g_vkMutex`. Registry tokens contain **only atomic CPU evidence**; attempts never retain the token.
  Each attempt owns two immutable descriptor sets, its pipeline, coherent mapped storage buffers and budget.
  `Inspect` forbids marker reads before retirement or after invalidation. A matching cookie after retirement
  publishes the last complete execution of that serial recording; reset without execution discards it.
- `DlssNrFeature_Vk.cpp`: optional original snapshot immediately before encode, processed snapshot only after
  successful resolve. Scope cancellation covers every intervening early return. Polling is after the existing
  disabled and picture-selection gates under the existing mutex. `FreeRouteObjects` polls while the device
  identity is still valid; capture allocations retain their own device identity after the model is released.
  Both `ShutdownVk(false)` and `AfterUpscaleDeviceGone` invalidate before registry forgetting. The latter
  notifies captures even when `g_vk.device` was already cleared by hand-back.
- `DlssNr_PairCaptureWriter.h` supplies CPU jobs to the **existing single writer in `DlssNr_PairCapture.cpp`**;
  no second writer translation unit was necessary. Paths, naming, temporary writes, rotation, PNG/PFM and
  model metadata remain shared. CPU vectors allocate outside the route lock; validation and two bounded
  memcpy operations run inside it; encoding and disk I/O run outside. No mapped pointer escapes the helper.
  An already copied CPU snapshot may finish after device teardown; a queued, uncopied one fails discarded.
- A broker mutex plus atomic backend identity freezes each request and serializes admission across APIs.
  No path holds that mutex while taking `g_vkMutex`; route/worker settlement takes route then broker.
  `ReleaseIfIdle` checks the phase inside the broker lock so a stale completion cannot release a new request.
  The menu enables the native branch only for the running after-upscale route. Native present retains a refusal.
  The existing trigger-file polling remains **D3D12 only**; native Vulkan is requested from the menu. No new
  polling filesystem operation or config key was added.
- `dlssnr_pair_capture.hlsl` is our own `Texture2D<float4>.Load` to `RWStructuredBuffer<float4>` shader, with
  an 8-byte width/height push constant and 8x8 groups. SPIR-V and byte header were generated by the documented
  recipe with native Linux DXC, independently of the shared Wine/MSVC prefix. No donor code/bytecode or DLL.
  Both source boundaries retain GENERAL; all queue-family indices are ignored. Producer writes precede reads;
  snapshot reads precede later image writes; private writes and the final update marker become host-visible.
- Explicit format set: RGBA8/BGRA8 UNORM display data, RGBA16F/RGBA32F display or the route's established
  linear-HDR domain. The writer receives RGBA32F numerical rows, emits PNG8 or PFM, and records actual VkFormat
  plus `sampled-RGBA32F-numeric`. sRGB, packed/PQ and unknown formats are refused. The capture inherits the
  route's domain classification; it does not discover a game's unreported transfer function. FP32 avoids the
  old FP16 keep image; NaN payload preservation and raw-byte equivalence are not promised.
- The **512 MiB** reservation counts actual Vulkan memory requirements, two CPU snapshots, bounded conversion
  rows and PNG block scratch. Device storage-range and dispatch limits are checked before commands. Opaque
  driver pipeline bookkeeping and general-purpose allocator metadata are not measurable under this cap.
  Four retained attempts maximum; no eviction or preallocation. Lost/unproven GPU allocations stay charged
  for the process even after metadata is discarded: unknown driver reclamation cannot fund another allocation.
  This may refuse later captures after device loss. Live drained teardown destroys children before the device.
- When quiescent the route polling gate is one relaxed atomic load; no clock, allocation, locks beyond the
  existing route lock, descriptors, dispatches, readbacks, thread wakeups or file work. Once requested, work
  retires on route polls/teardown; NR disabled leaves pending objects retained and never manufactures completion.

### Evidence and remaining qualification

`tests/nr-capture` exercises the shared split Session and unchanged D3D12 output, including PNG/PFM and privacy.
`tests/nr-vk-pair-capture` compiles the production backend against fake Vulkan commands, separates recording
from execution, checks black pixels, replay, allocation failure/budget, cancellation, discarded recordings and
queued writer/device loss. Six mutants individually fail assertions (retirement, invalidation, marker, budget,
simultaneous reuse, failed tracking). The inherited recording suite covers registry lifecycle hooks.
The adjacent delivery report contains exact command results, shader hashes and the single Release build result.

The host fakes do **not** execute SPIR-V, validate driver synchronization, or establish native Windows/Proton
performance. The GPU/game matrix above remains required: FP16/FP32/UNORM analytic values, HDR values beyond
one, zero/NaN/Inf where representable, validation-layer barrier checks, real recording reuse, delayed retirement,
route switch, loss and queued-writer teardown. No game was launched and no binary was installed by this change.
