# Native Vulkan sequential model passes

Design and implementation slices, 2026-11-19, initially based on `6972e2d7`. **The portable decisions
and creation-observation adapter are implemented; the runtime chain is not. Native Vulkan still executes one pass with master settings.** No GPU run.

## Why the existing route has one pass

This is a renderer integration gap, not a demonstrated restriction of the model. The
[bounded chain](bounded-chain.md) explicitly limits native Vulkan to one/master. In
`DlssNrFeature_Vk.cpp`, `VkState` owns one `feature` and one `output`; `EvaluateAfterUpscaleVk`
creates that feature and evaluates it in the same command buffer, using the master config fields.
There is no pass snapshot, secondary output, per-pass feature/history or chain budget query there.
The menu's `RenderPassControls` returns before the count/individual controls on Vulkan or UseProxy.

The D3D12 guarantees cannot be obtained merely by looping that evaluate:

- `VkRecording::Registry` observes allocation, Begin, End, Reset and Free. Its pins survive executable
  recordings, including unsubmitted and replayable work, which is correct for resource lifetime.
  It has no submission/completion result. Reset/Free also retire a recording that was **never
  submitted**; a released pin therefore cannot certify feature initialization.
- `DlssNr_VkPresentHost.cpp` hooks `vkQueueSubmit` and `vkQueueSubmit2` through `LockedQueueCall` for
  queue serialization. They do not report the game's submissions to the recording registry.
  `VkPresentModel::Submitted/Completed` refer to the present host's private recordings and fences,
  not the after-upscale command buffer. Borrowing its most recent completed ticket is invalid.
- `RouteRecordingBusy`, resize and `ShutdownVk` already protect recorded generations. A successful
  `vkDeviceWaitIdle` cannot execute a command buffer that the game has not submitted, nor prevent its
  future replay. Waiting idle immediately after feature creation cannot be the creation gate.
- `AskModelVramVk` reports model allocation statistics. It is not a live device-local heap budget
  and cannot substitute for the D3D12 chain's adapter budget/current usage admission.

These missing adapters cross queue observation, native handle lifetime and device-loss handling.
This delivery takes the task's permitted first-safe-slice option rather than weakening the creation
gate or changing the single-pass renderer. No claim that a full implementation is impossible follows.

An alternative is private creation work: the present model's builder already creates on its own
command buffer, submits through the host and waits for its fence. Extracting that builder could avoid
observing the game's **creation** submission, but would still need after-upscale queue/owner admission,
per-pass settings and job cancellation/abandon, plus ordering of later game-owned evaluations. The
present owner's feature cannot simply be borrowed while the after-upscale owner is active. This is a
viable follow-up design choice, not evidence that today's after-upscale feature has that fence.

## Contract and mapping for the runtime integration

| Requirement | Vulkan mapping |
|---|---|
| Off path | `Passes=1` takes exactly today's `EvaluateAfterUpscaleVk` body, even with saved individual settings. No new tracker, heap query, allocation or model arguments. Disabled NR remains unchanged. |
| Route | First version is native after-upscale only: Stage 0, no proxy. Before-upscale, present, bridge, RR and cadence integration are out. Their existing paths remain as they are. |
| Settings | Snapshot through `Config::GetDlssNrPassSnapshot`; existing `Passes`, `IndividualPassSettings` and `[DlssNr.PassN]`, no new key. Counts bound to 1..10; one resolved tuning record per feature. Shared working size/filter and composition settings. |
| Histories | Chain generation in `VkState` owns one NGX feature per requested pass. Rebuild only changed instances after safe retirement; rebuild of an upstream feature resets all downstream histories. A reset from the game, picture epoch, stage switch or NR re-enable resets all active histories. |
| Outputs | Keep the original proxy immutable. Two distinct working outputs alternate using `Chain::Routing`; pass N reads the last successful output and writes the other. Neither may alias the proxy. Encode, meter and compose run once. |
| Barriers | Both model inputs/outputs use GENERAL as the forwarder expects. Before each extra evaluate, record compute shader write-to-read and prior-read-to-write dependencies, including when the layout stays GENERAL. Do not infer execution order from an `OwnedImage.layout` value. Restore intermediates and the final resolve's layouts as the existing route does. |
| Partial failure | Stop on the first failed create/evaluate or admission. Never select its output. Compose the last successful answer, or leave the game output untouched if no pass succeeded. Retain failed features until safe retirement; a fault/device-loss must not dispatch composition on the lost device. |
| Creation | Record at most one create per observed present, at least 500 ms apart, after settings and dimensions/format have settled 500 ms (`Chain::Schedule`). New feature evaluation requires a later present AND successful submission and completion of its exact creation recording. Discard/failure/unknown completion cannot become success with time or Retry. |
| Ordering | First version requires ONE_TIME_SUBMIT for creation and refuses simultaneous-use or unknown recording flags, duplicate evaluation per present and unproved queue order. New generations require old recording pins retired plus a successful live-device drain. This is conservative and can pause NR; skipped work must not be reported as speedup. |
| Budget | Before each generation/extra model, query the relevant device-local heap with enabled/supported `VK_EXT_memory_budget`. Refuse unknown/mixed heap attribution. Reserve max(512 MiB, 20% budget) plus 150% of max(measured model bytes, 256 MiB) and conservative allocated texture storage. Refuse overflow. Model stats are only an estimate; allocation can still fail after admission. |
| Lifetime | Include every feature and ping image in the existing route generation pin. Never release at failure or settings change while recorded work can reference it. Live-device drain follows recording quiescence; failed drain/device loss abandons handles, with no Vulkan or NGX destructor. Extend `FreeRouteObjects`, `ShutdownVk(false)` and present-owner hand-back together. Preserve NR/owner/queue lock order. |

No shader or cbuffer change is needed for sequential model evaluation. Supersampling downsamples only
the selected final answer, exactly once, and resolves it against the matching native proxy.

## First safe slice in this revision

`DlssNr_VkChain.h` has no Vulkan types. It supplies route/prerequisite decisions, an explicit
creation-evidence state machine tied to `(device, buffer, epoch)` and submission serial, latched
per-settings failure, and overflow-safe budget admission. Routing and settling remain the existing
`Chain::Routing` and `Chain::Schedule`, tested together with these decisions. The creation proof is
**not** a resource lifetime/release oracle: completed command buffers can remain replayable.

The header is intentionally not connected to the renderer yet. Its support flag is evidence supplied
by a future adapter, not a user setting and not inferred from pins. `nr-vk-passes` exercises cancelled
creation, failed submit, stale completion, re-submission, handle reuse, unsupported routes, unknown
budget, all ten output positions and settings retry. The real recording registry demonstrates that
both submitted-unknown and never-submitted recordings can drop their pins on Reset. Source guards
hold the current renderer and menu to one pass while this slice is the only implementation.

Implementation sequence (steps 1–2 adapters are implemented below; steps 3–4 remain):

1. Observe successful submit/submit2/KHR of the exact primary creation buffer, with an owned completion
   primitive and identity protected against fence reuse. Cover entrypoint aliases, failed submits,
   multiple queues, command-buffer reset/free/reuse and device loss. Refuse secondary/simultaneous
   or unobserved paths. Never call Vulkan or acquire the NR mutex while holding the registry mutex.
2. Feed live heap budget/usage and memory-type attribution. Bound retained generations and make
   refusal recover only on a meaningful settings/generation change, not per-frame allocation storms.
3. Integrate the chain generation, feature ramp, ping-pong barriers, partial composition and all
   release/abandon paths. Add runtime logs with requested/completed counts, creation epoch/submit
   serial, gate reason, budget/usage and failed pass. These logs do not exist in this slice.
4. Only then expose the after-upscale Vulkan controls. Keep the one/master message on present,
   before-upscale and proxy routes; update locale packs and executed menu states at the same time.

## Follow-up implementation, 2026-11-19

Step 1a now publishes a bounded, allocation-free per-device hook certificate. Every required
lifecycle, queue retrieval and submit/submit2/KHR entrypoint must attach successfully; failures,
missing pointers and target saturation deny chain admission. Repeating installation cannot upgrade
a partial certificate on a live device. `Passes=1` does not publish or consult it; the submit
wrappers preserve their inactive forwarding boundary. A request above one without coverage now stands down before NR recording.
Step 1b now retains allocation level in the recording ticket (including the independent observer,
w/Dx12 and host-owned allocation paths). Creation evidence refuses secondary and unknown levels.
Step 1c now provides `DlssNr_VkCreation.h`: ten recording slots and private fences per observed
device, with bounded device slots. It captures exact recording matches before forwarding the
application submit and preserves its result. A successful submit gets a non-null, empty marker
submission with an owned fence on the same locked queue. Polling its signal supplies the matching
epoch/serial to `VkChain::Creation`; a borrowed application fence is never read or reset.
[The Vulkan fence scope](https://docs.vulkan.org/spec/latest/chapters/synchronization.html) includes
earlier commands on that queue. Other queues do not share this proof.

The adapter is ready for the chain generation to arm; the single-feature renderer is **not**
converted into a pending-feature holder in this slice. That belongs to step 3: publishing an
uncompleted feature into the legacy `g_vk.feature` would let a switch to Passes=1 evaluate it
without its completion gate. The generation must retain pending features separately and cannot
lend them to the legacy path. No multipass feature or menu control is available yet.

Creation now requires a primary, serial [ONE_TIME_SUBMIT recording](https://docs.vulkan.org/refpages/latest/refpages/source/VkCommandBufferUsageFlagBits.html). Completion of an ordinary
replayable recording alone cannot prevent its later creation commands racing already-recorded
evaluations. Such recordings are conservatively refused. Reset/free/pool retirement/reuse,
replay (even after completion), failed submits, cancellation and off intervals invalidate proof.
Failed marker submits retain their fence until signal or a proven live drain; unknown completion
never becomes readiness. Device loss makes all later polling refuse and device teardown abandons
without any Vulkan destructor. Successful live-device drain allows final fence destruction.
All pins contain CPU metadata only, and a completed fence never authorizes releasing the feature.

Nested callbacks on the same queue are one observation; nesting on another queue invalidates the
unobserved interval. Protected queues and multi-device groups are not admitted. No observer is
registered without its device certificate. The inactive wrappers use the same queue lock and
forward the same arguments once; they take **no observer lock**, allocate nothing and submit no
marker. An already armed device switched to Passes=1 only invalidates old evidence atomically.
Its pending objects remain bounded until active polling or externally synchronized destruction.

The certificate has 16 process-wide slots, no map or lazy heap initialization. It is published
under the existing installation lock, read with atomics, and cleared at device destruction.
A refusal is logged once at publication; global capacity exhaustion logs once and refuses
all additional devices until slots retire. Native queue functions hidden behind an unqualified
own-module wrapper are conservatively uncertified. The existing host install opt-in remains.

## Step 2: restricted live heap budget, 2026-11-19

The lead approved the unique-device-local-heap policy after the attribution review below.
`DlssNr_VkBudget.h` now queries `vkGetPhysicalDeviceMemoryProperties2` with
`VK_EXT_memory_budget`, enabled at successful device creation only for a requested chain and
only when supported with the properties2 dependency. Without that opt-in/certificate the source
is unavailable. The query is performed anew for each admitted attempt; zero/unwritten/invalid
output, backwards time, a query taking more than 100 ms, a sample older than 100 ms or device loss
refuses. A query does not promise an allocation will succeed: budget and usage are driver estimates.

Exactly one DEVICE_LOCAL heap is required. Every chain texture reservation must have a known
actual allocation type on that heap and nonzero `VkMemoryRequirements::size` (or a proved larger
bound). Different/unknown/nonlocal types refuse. The **whole** estimated model byte count is charged
to this heap; existing texture storage may also be counted again on top of usage. This is an
intentional over-estimate, allowed only by this restricted policy. It is not model allocation
telemetry and never infers a heap from the model's stats. Multiple local heaps are refused.
Overflow-safe headroom remains max(512 MiB, 20%) plus 150% of max(model estimate, 256 MiB) plus all
texture reservations.

`BudgetGate` latches a named refusal until device generation, dimensions/format/filter, pass count
or effective tuning changes. Clock, budget fluctuations, recording resets, inactive pass overrides
and inheritance switches with identical resolved tuning do not retry. `AdmitChainBudget` logs the
reason once per such change. Passes=1 returns before source lookup/query or latch mutation. The
device-create extension helper does not allocate or alter the extension list when unrequested.

`RetainedGenerations` supplies two fixed slots for the step-3 owner, including abandoned generations.
A slot can be released only after both recording retirement and live drain; abandoned slots never
return. The generation integration must reserve **before** allocating/recording and preserve this
owner across changes. These metadata tokens themselves release no GPU resource.

The host configures the live budget source alongside the creation observer and exposes admission
and post-create sampling. The single-feature renderer still does not call them; there is no chain
generation yet. This is the step-2 adapter, not an operational multipass renderer.

The native fixture and RTX 4090 DOOM run must log heap index, live budget and usage immediately
before and after **each** created pass, together with full model estimate and texture reservation.
Record before/after both success and failure, and show that the next attempt uses a fresh query.
Demonstrate refusal for two local heaps/missing extension/stale query/mixed textures in the fake
fixture, and that unchanged refusals do not query/allocate each frame. Compare actual usage deltas
with the intentional over-estimate; do not call the aggregate model statistic a per-pass delta.
No GPU run was performed here.

## Historical step-2 attribution review (resolved by the restricted policy above), 2026-11-19

At this historical review, step 1a/1b/1c was implemented and host-tested. Step 2 was **not** implemented; steps 3/4 were not
started. The unresolved contract is what proves the relevant heap for the opaque model's
allocations before the first chain feature can be created. This is an attribution boundary,
not a failure of Vulkan's live budget query.

`AskModelVramVk` and the forwarder's `dlssnr_vk_query_stats` expose only an aggregate byte count,
covering all features and APIs. `dlssnr_vk_create` supplies the model a command buffer and a
parameter block, with no allocator/memory-type reporting callback in this integration. The
fork's `CreateImageOn` chooses a device-local type from each image's actual requirements; its
`OwnedImage` does not retain that type or allocation size. Its type is not evidence for the
model's allocations. There is no certified model allocation observer here.

The executable `budget_boundary.py` counterexample uses the actual `FindMemoryTypeIndexOn`:
two valid device-local heaps and different requirement masks choose different heaps. Identical
model statistics admit on one and refuse on the other; summing budgets masks the refusal.
It demonstrates why neither aggregate bytes nor the first image's heap identifies the relevant
heap. It makes **no claim** that a particular model or GPU uses those two heaps.

The task requires refusing unknown/mixed attribution. I therefore did not substitute physical
VRAM, aggregate heap budgets, elapsed time, or an assumed model allocation policy. A live query
alone would not close this contract. To continue, establish either a model allocation contract
or a complete allocation observation path, including allocations performed after create or on
another thread. Alternatively, the lead must decide whether a restricted unique-device-local-heap
policy is acceptable: charging all opaque model bytes to that heap is conservative for local
VRAM, but does not attribute nonlocal allocations or turn aggregate stats into per-feature facts.
That restriction is a proposed clarification, **not** an implemented or qualified policy.

Until this is resolved there is no live budget adapter, retained chain generation, pass ramp,
ping-pong execution or new menu. Passes=1 remains the legacy route; requests above one cannot
run a chain. Step-1 completion infrastructure is not armed by the single-feature renderer.

## Initial step-1 safety review (superseded by the follow-up above), 2026-11-19

The initial review at `bbdda337` started at `44ac22cb` and stopped **before completing step 1**. No submission
observer, owned completion primitive, heap adapter, runtime chain or menu control was added.
The stop is a missing proof in this integration, not a claim that Vulkan cannot implement it.
The following executable witnesses make the prerequisites more specific than the first slice:

- `tests/nr-vk-recording/install_test.py` executes the production `HookDevice`, `AttachTarget`
  and lifecycle callbacks. `partial-install-is-not-lifetime-proof` rejects just both
  `vkResetCommandBuffer` detours while allocation, Begin and End still succeed. The real
  registry admits the recording, and a successful driver Reset leaves its pin alive. Only
  the next observed Begin retires it. Therefore neither a ticket nor a live pin certifies
  complete lifecycle coverage. `AttachTarget` currently returns `void`; its failures and
  four-address limit produce logs, not per-device admission evidence.
- `serial-secondary-is-not-primary-proof` executes allocation and Begin for both levels.
  Both yield `Reuse::Serial` and accept pins. `Ticket` does not retain allocation level:
  serial use proves neither primary level nor that a submitted primary is this recording.
  A future observer must refuse secondary/unknown creation before recording it; matching
  a secondary's numeric handle against the top-level submit list is not an alternative.
- `tests/nr-vk-passes/submit_boundary.py` compiles the production `LockedQueueCall`,
  `hkQueueSubmit` and `hkQueueSubmit2` bodies with fake driver boundaries. Its 27 cases
  cover two known queues and an unknown queue, three result classes and legacy/modern
  callbacks (two modern slots). They preserve the exact arguments/result and call down
  once; no result feeds `VkChain::Creation`. The KHR-to-shared-callback attachment is a
  source guard only. This does not exercise real Detours or prove native alias coverage.

These are **characterization tests of the blockers**, not tests accepting a completion
adapter. Replace their missing-capability assertions with refusal/success assertions when
that adapter is implemented. The existing policy mutants still test only portable decisions.
No runtime source was changed, so the one-pass/off behavior has no new tracker or allocation.

The first implementation commit still needs one coherent ownership boundary:

1. Publish per-device coverage of allocation level, Begin/End, reset/free/pool/device
   retirement, queue identity and all submit entrypoints. A failed/missing/overflowed hook
   must deny chain admission; preserve the legacy one-pass path. Scope nested export/ICD
   callbacks so one native submission is not mistaken for a replay. Resolve the interaction
   with w/Dx12's own lifecycle notifications explicitly.
2. Arm an exact primary serial recording while it is open, before feature creation can
   record GPU work. Invalidate on observed discard/reuse/failure and on any coverage loss.
   Capture submission identity before forwarding; a borrowed application fence may be
   reset/reused and cannot be the owned proof. Multiple queues must not share a "last
   completed" fact. Keep the registry mutex out of Vulkan calls and NR-lock acquisition.
3. Give the adapter its own bounded completion objects and device lifetime. Specify creation,
   submission failure, marker failure, pending cancellation, polling, successful retirement
   and device-loss abandonment together. A future fence-only marker needs ordering on the
   actual submitting queue under that queue's lock; use a non-null submit structure for
   compatibility with the bundle. A signalled marker proves execution, not retirement of
   a replayable recording. Do not wire the creation gate until all these paths are exercised.

The host does have private present fences and a drain/abandon path. Those objects cover its
own work, and their existence does not give ownership or completion to an after-upscale
creation buffer. No resource was added speculatively to that teardown in this review.
Steps 2, 3 and 4 remain unstarted, in their original dependency order.

NeuRotic's `VulkanNrPassPlan.h` and `VulkanNrPassRecorder.cpp` were read for the idea of an explicit
preparation/completion boundary and ordered delivery. No source, tables, strings or data were copied.
Its layer-specific scales/composition and all-or-nothing routes are deliberately not adopted here.

## Integration and GPU qualification (lead only)

Run serially after integrating and building, never against a running game's DLL. Existing regression
commands (they do **not** prove a chain that is not yet wired):

```sh
./build-local.sh Release > x64/nr-vk-passes-build.log 2>&1
python3 tests/guide-match-vk/run.py
python3 tests/vulkan-present-host/run.py
python3 tests/vulkan-overlay/run.py --ref HEAD
```

The guide fixture proves resampling and barriers, the present fixture protects ownership hand-back,
and overlay protects adjacent queue/teardown behavior. Their own prerequisites and SKIP rules apply.
An after-upscale chain fixture still has to be added. Its first phase must test the step-1
adapter independently: distinct and aliased export/device/KHR targets, nested dispatch, one
failed lifecycle detour, hook-slot exhaustion, secondary/unknown/simultaneous refusal, two
queues completing in reverse order, a delayed/failed application submit, a failed private
marker submit, borrowed-fence reuse, command-buffer/pool reset/free/reallocation and device
loss while a private completion is pending. Assert the exact creation epoch and submit serial,
no readiness from another queue and no GPU destructor on abandonment. Test both bundle-on
and independent-observer modes. Then add real NGX, validation and synchronization validation
demonstrably enabled, discard, retained command-buffer replay, three in flight,
count/settings/size changes and partial failure. No existing command
above substitutes for that missing fixture.

For the current revision, DOOM Eternal is only a one/master regression run: native Vulkan
DLSS, Stage 0, NR enabled at device creation, record DLL/model hashes, Proton/driver and exact
INI. Passes 2/3/10 still have unavailable chain controls and cannot run a chain; uncertified devices refuse NR, while certified devices retain the one/master route;
there are no creation-completion or chain-budget logs to assert. This worker ran no game.

Only **after steps 1 through 4 and the missing fixture pass**, run the chain procedure in
DOOM Eternal under Proton on RTX 4090 with the same recorded environment and hashes:
first Passes 1, then 2, 3, 10 and 1.
Keep a moving scene and HUD visible; change pass 2's preset/intensity independently, return to master,
change working scale 1/0.5/1.5, resize/alt-tab and disable/re-enable NR. Expect one create each 500 ms
at most, each first evaluate after its logged creation completion, completed <= requested and a
budget refusal naming the pass. Deliberately fail pass 2/3 in the fixture: the prior successful
answer must compose, failed output must never reach the game, and unchanged settings must not retry.
Check validation, image A/B, requested versus actual model work, GPU timings, VRAM and teardown;
counting frames or seeing `feature up` alone is insufficient. One-pass off-path commands/pixels must
match the baseline. Present/stage-1 controls stay unavailable. Native Windows remains unqualified
until separately tested. In this revision only Passes=1 is the unchanged one/master path; no multipass claim.
