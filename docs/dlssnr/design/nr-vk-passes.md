# Native Vulkan sequential model passes

Design and first safe slice, 2026-11-19, based on `6972e2d7`. **The portable decisions are implemented;
the runtime chain is not. Native Vulkan still executes one pass with master settings.** No GPU run.

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
| Ordering | First version refuses simultaneous-use or unknown recording flags, duplicate evaluation per present and unproved queue order. New generations require old recording pins retired plus a successful live-device drain. This is conservative and can pause NR; skipped work must not be reported as speedup. |
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

Remaining implementation, in dependency order:

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
An after-upscale chain fixture still has to be added: real NGX, validation and synchronization
validation demonstrably enabled, delayed/failed submit, discard, retained command-buffer replay,
three in flight, count/settings/size changes, partial failure and device loss. No existing command
above substitutes for that missing fixture.

Then DOOM Eternal under Proton on RTX 4090, native Vulkan DLSS and Stage 0, NR enabled at device
creation: record DLL/model hashes, Proton/driver and exact INI, first at Passes 1, then 2, 3, 10 and 1.
Keep a moving scene and HUD visible; change pass 2's preset/intensity independently, return to master,
change working scale 1/0.5/1.5, resize/alt-tab and disable/re-enable NR. Expect one create each 500 ms
at most, each first evaluate after its logged creation completion, completed <= requested and a
budget refusal naming the pass. Deliberately fail pass 2/3 in the fixture: the prior successful
answer must compose, failed output must never reach the game, and unchanged settings must not retry.
Check validation, image A/B, requested versus actual model work, GPU timings, VRAM and teardown;
counting frames or seeing `feature up` alone is insufficient. One-pass off-path commands/pixels must
match the baseline. Present/stage-1 controls stay unavailable. Native Windows remains unqualified
until separately tested. In this revision DOOM must still run exactly one/master; no multipass claim.
