# Bounded in-flight NR recordings

Design before implementation, 2026-10-19. Replaces the admission rule of
[bounded-chain.md](bounded-chain.md) ("all preceding NR usage must be Ready") and relaxes the retirement
rule of [submission-tracking.md](submission-tracking.md) ("no timer, frame-count or 32-evaluation fallback").
Applies only to tracked recordings: `Passes`/`RRPasses` above 1 or `IndividualPassSettings`. The default
one/master path is unchanged.

## What went wrong in game

A tracked recording was admitted only when every earlier NR recording was Ready. Ready means two
things: the GPU fences completed, and the game's command list was successfully Reset. The Reset is
there to guard against replay, because D3D12 lets a closed list execute again.

Games rotate their command lists, so a list is Reset only when it is reused, two or three frames
later. The pass was therefore skipped on the frames in between, and when the game stopped using a
list it was never Reset again.

- **Cyberpunk 2077**, 2026-10-19, `e5f592de`, after Ray Reconstruction, `RRPasses=3`
  (`OptiScaler_15314481169.log`):
  - Before a pause the model ran on 94 of 300 evaluations.
  - The pause menu opened at 19:36:33 and the game came back at 19:36:43. From then on `model_ok`
    stayed at 427 for the rest of the session: "Waiting for previous NR recording Reset and all GPU
    fences".
- **Crimson Desert**, 2026-10-19, `cd568080`, `VitEvery=2`, present route:
  - 941 skips over 2854 base frames, so NR was on two frames in three.
  - This alternation is at least a candidate cause of the shimmer reported in
    [vit-skip.md](vit-skip.md) section 10.

The design note itself called this "conservative serialization, not a high-throughput resource-ring
implementation".

## What the gate actually protected

Mapped from the source on 2026-10-19 (file:line references are in the review notes).

- **Extra passes add no CPU-written GPU state.** Passes 2 and 3 record only barriers, `SetExtras` and
  `evaluate`. All descriptor and constant writes are the single path's own: 64 shader-visible heaps of 8
  slots with 64 mapped upload buffers, taken round-robin with no fence, about six dispatches a frame,
  so roughly ten frames of coverage. The sub-pass rings are 16 to 112 slots.
- **Cross-frame state is GPU-only.** This covers the NGX histories, `passPing`, the cadence store and
  the stabilizer and UI-mask anchors. It is ordered by queue order and the recorded barriers, exactly
  as on the single path.
- **`Ready(g_usage)` gates exactly one decision:** whether a new recording may start. Retirement uses
  per-object snapshots; creation spacing uses `Completed`.
- **The single path has no completion evidence at all.** It frees after 32 evaluations, and its rings
  assume the GPU is less than about ten frames behind. (Amended 2026-11-17, unvalidated on GPU:
  `DlssNr_UntrackedRetire.h` keeps the 32-evaluation countdown on the untracked D3D12 path and adds a
  per-device `ID3D12Fence` signaled on the swapchain's present queue after each successful `Present`,
  freeing only once `GetCompletedValue() >= parkedSerial + 1` as well; `UINT64_MAX` and device removal
  retain, while a live device with no single present-queue fence falls back to the 32-evaluation count
  and logs once at `INFO`. Descriptor/constant rings still have no fence of their own, and off-screen
  recordings never presented, replayed lists or un-synchronized queues remain outside a present-fence
  argument.)

So the Reset requirement did not protect any state that the chain adds. Its one benefit was replay
protection, and the cost was the throughput loss and the permanent stall shown above.

## What others do

- **Fences, not Resets.** Microsoft's
  [fence-based resource management](https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management)
  pattern rings CPU-written data per frame in flight and reuses a slot once its fence has completed.
  [DirectX-Specs](https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html) requires GPU
  completion before freeing a resource or rewriting descriptors it references.
- **Deferred release by fence.**
  [Diligent](https://diligentgraphics.com/diligent-engine/architecture/d3d12/managing-resource-lifetimes/)
  queues released objects with the next command-list number. ReShade keeps one allocator and fence per
  back buffer and waits only when that slot's fence has not completed.
- **No one guards against the host replaying its lists.** Streamline makes the host declare a
  `ResourceLifecycle` for each tagged resource. The fork by wilsjo2 that runs the same multi-pass
  chain in game keeps persistent per-layer features, ping-pongs answers through two textures, creates
  one feature per submitted frame and parks replaced objects for deferred release.

## New rules

All in the pure model (`DlssNr_SubmissionModel.h`), host-tested. Revised twice after Codex reviews: the first
draft let elapsed ticks free resources, checked queues only after admitting an overlap, pruned replay evidence at
admission and counted epochs instead of recordings; the second still let a late first submission run over
rewritten ring slots, proceeded unordered after a bounded CPU wait, and freed resources inside the replay window.

**Evidence, never mixed with time.**
- *Safe* (any mode): Ready; cancelled (Reset with no execution ever seen, so its commands are gone); or released
  -- only our reference to the list is left (`AddRef`/`Release` probe on the list we keep alive), so nobody can
  execute it again -- and either completed or never executed.
- *Admission-done*: safe, or outside strict mode complete (submitted, nothing pending, every fence reached) though
  the list could replay -- the trust Streamline, ReShade and NGX place in the host.
- *Finished* (may free resources): safe, or outside strict mode complete AND aged out of the replay window.
- *Unanswerable*: no fence ever will answer -- a failed Signal, or a recording never submitted that the game still
  holds. Ticks never turn anything unanswerable or outstanding into finished.

**Clock.** `Submission::Advance` keeps a monotonic count passed by the renderer, which advances it on every NR
entry (`Dispatch`, and the before-upscale scope) before anything can return. Tracking an epoch stamps it.
`AgeOutTicks` = 128, `RingTicks` = 8.

**Lifetime set.** `g_usage` drops finished epochs; outside strict mode, when fewer than 8 of its 64 slots are
free, also the oldest admission-done ones (their replay window is cut short rather than refusing every later
recording for want of a slot). Retirement snapshots copy this set.

**Admission** of a new tracked recording into list L, over the lifetime set, without modifying it:
- strict mode: refused while anything is not Ready (the original rule);
- refused if L already holds an unsubmitted NR recording (a second one would reuse its ring slots before the
  first ran); the explicit pre-upscale lease still borrows its own recording;
- refused while any recording the GPU may still read is `RingTicks` old -- waiting for its first submission, or
  submitted and not done: eight entries cannot wrap the main pass's 64-slot ring (six slots an entry) or the
  sub-pass rings (16 to 112 slots) over a recording that has not finished. It clears when that recording is done,
  Reset (cancelled) or released by the game;
- an epoch whose Signal failed is treated like any outstanding recording: it ends in `Behind` and stops tracked
  NR rather than risk its ring slots (in practice the device is gone when a Signal fails);
- serialized (refused while anything earlier is unfinished) until one queue is established, or for good once a
  second queue was seen (below); refused if unfinished work sits on more than one queue;
- otherwise admitted while at most three earlier recordings are unfinished.

**Queue affinity.** Admission cannot know which queue the game will execute the new list on; the submission hook
sees it afterwards. Recordings may overlap only after eight tracked chain batches all went to one queue (only
chain recordings count; the module's own present and measurement lists do not). When a chain batch goes to a
second queue, or to one the tracker could not admit, admission is serialized for the session and a warning
says whether the previous queue had finished. Nothing is injected into the game's queues to order the two: a
GPU wait there can turn a dependency only the game knows into a cycle (Codex, third review). The cost is that
recordings admitted before an unannounced switch -- up to three, the in-flight limit -- may overlap one still
running on the other queue.
Games run DLSS on one direct queue.

**Retirement.** A tracked parked object is freed when every epoch in its snapshot is finished AND the clock
advanced 32 ticks since parking (covering work NGX may keep on queues of its own). Because completed-unsealed
epochs only finish after the replay window, a replay inside it still finds its resources alive and latches strict
mode before they could go. An object waiting after 128 ticks only on aged unanswerable epochs is stuck --
recomputed every tick, so a late submission returns it to the normal rule: retained, never freed without
evidence, and the first 64 such do not count toward the parking cap of 32. At shutdown only Ready frees.

**Replay.** The model counts executions per epoch, including duplicates inside one `ExecuteCommandLists` batch;
the first second execution without a Reset latches strict mode for the session (admission and retirement back to
Ready, no replay window, no capacity pruning), with a warning. Strict mode is containment, not recovery: a replay
of an epoch already outside the window is not protected.

**Tracker capacity.** A slot of the 256-epoch tracker can be reclaimed from a safe epoch, or, outside strict
mode, a complete unsealed one aged 128 ticks, provided no Usage references it.

**Rebuild headroom.** In game on 2026-10-19 (`RRPasses=3`, `RRWorkingScale=2.0`), the fixed chain ran 3/3 on 94%
of evaluations through a pause, then a size change needed a new generation and the adapter-headroom admission
refused it: the usage it measured still held the three features being replaced (about 1.8 GB each at 2.0), and a
refusal latched NR off for the session. Now a refused generation first parks the current one -- features, pass
features, scratch, as a resolution-change rebuild does -- and the pass skips with "Chain generation waits for the
previous one to be freed" until retirement frees it (finished plus 32 ticks; completed-unsealed epochs finish
after the 128-tick replay window, so a few seconds at low frame rates) and admission passes. Only a refusal with
no current generation and nothing non-stuck left parked latches the failure, as before.

**Unchanged.** Creation spacing, the 500 ms settling, one evaluation per present.

## Residual risk, stated

- A replay later than the 128-tick window (or after capacity pruning) is not protected: the same trust
  Streamline, ReShade and NGX place in the host. Shutdown cannot check epochs already pruned for that reason.
- Up to three recordings admitted before the game first moves NR work to a second queue may overlap one still
  running on the first; after that, everything is serialized. Ordering it would mean injecting a dependency into
  the game's queues.
- A game that keeps a recorded list alive, never submitting, resetting or releasing it, stops tracked NR (status:
  "An NR recording is still waiting for the game to submit it"); the alternative is running over its slots.
- Unanswerable epochs never leave the lifetime set and keep anything parked after them stuck; more than 64 stuck
  objects block rebuilds again. Not observed; the log says so if it happens.
- The supersampling scalers (`OS_Dx12`, upstream's class) keep one upload constant buffer each, effectively ring
  depth 1; their constants change only with sizes, and this is unchanged from the single path.
- NGX's internal resources are opaque; NGX is built for frames in flight, and nothing here proves more.
- The release probe trusts COM reference counts: a count of one cannot be faked upward by anyone else, but an
  internal reference held by a layer we do not know about only makes it more conservative.

## Validation

- **Host.**
  - New model cases in `tests/nr-submission`: Complete-but-unsealed admits; the in-flight limit;
    mixed-queue refusal; age-out of never-submitted and unknown epochs; the replay latch; reclaim.
  - `tests/nr-multipass` admission cases updated to the new rule.
  - The enlarger's tracked-retirement cases.
  - The full host tier.
- **Build.** Release.
- **In game.**
  - Cyberpunk 2077 with `RRPasses=3`, pausing mid-session. Expect `model_ok` to track base frames
    after the pause, and `requested=3 completed=3` throughout.
  - Crimson Desert re-run with multiple passes, to check that the alternation is gone.
  - Native Windows on Rafael's PC before calling it done (CLAUDE.md, "Proton is not a D3D12
    validator").

## Live opt-in after an untracked generation (2026-11-19)

The old `g_legacyRecorded` session latch prevented the first menu change from one pass to multiple passes,
individual settings or adaptive cadence from taking effect until restart. It was correct to refuse assigning
an empty tracked usage set to objects already used without tracking. The migration below replaces that refusal
only when the old recording history can be proved by the private D3D12 present host. Source/host validation is
not Windows, Proton or GPU qualification.

### Evidence and route boundary

`RunPresentPass` alone marks `PrivatePresentRecording`. `PresentSource`, a current hook setting, or the presence
of *some* swapchain fence is insufficient. Each untracked NR admission remembers the private list until
`SubmitPresentList` reports its actual submission (including a failed Signal), or `DropPresentList` reports
cancellation. A list not yet submitted cannot be made safe by waiting for unrelated presents. Duplicate pending
list identities and pending-slot overflow make the history unsupported.

The first submitted private list retains its queue and fence identities; subsequent legacy submissions must
use the same queue and fence. These two COM references are intentionally held for process lifetime, as in the
existing present-fence registry, so pointer recycling cannot fabricate queue identity. Migration waits for the
last legacy submission's fence value; `UINT64_MAX` never satisfies it. A failed Signal may be covered by a later
successful signal on that *same* queue, but cannot authorize release by itself. There are no new queue waits,
GPU commands, or submission hooks on the single/master path. Its commands, constants, configuration and model
settings are unchanged; it gains CPU provenance bookkeeping and a held composition reference per entry.

An untracked game-owned list (before/after upscale), the D3D11 bridge's present host, mixed private queues or an
unknown history retains a route-specific restart status. Mandatory `Dx11SceneStage` refuses that history;
non-mandatory opt-ins retain the previous single/master fallback. A scene-stage session which starts tracked
is unchanged. A mandatory scene-stage request may migrate an exclusively private-present legacy history;
the restriction concerns the old work, not the new tracked destination. Vulkan is unchanged. Supporting those other legacy routes requires their own completion AND
no-late-submission/replay argument; the generic present fence alone is not one.
(Amended 2026-11-20: a history made only on the upscaler's output no longer needs a restart. It is not migrated,
because that argument still does not exist for it; its generation is left in video memory instead. See "Live opt-in
from the upscaler's output" below. The before-upscale scope, the bridge's present host, mixed histories and
mandatory `Dx11SceneStage` keep the restart status.)

### Transition and resource inventory

Under the existing `g_nrMutex` and a valid recording lease:

1. The first supported opt-in stops all new untracked NR recording. If a private legacy list is still on the
   CPU, NR skips until that host submits or cancels it. Reverting the menu choice during this wait does not
   resume untracked writes. No other recording scope can begin the transition.
2. Detach the old generation while `g_tracked` is still false: every model/pass feature; current and alternate
   color/output/HDR/working/ping-pong textures; held frame; depth/motion clones, constant depth and matched
   guides; unpacked periphery surfaces; meter/calibration textures and their readbacks; both scalers;
   stabilizer, UI mask, cadence, enlargement, guide-match and periphery passes; the entire composition shader
   (including its descriptors and mapped constants); detail measurement copies/pass, pulse and motion probe.
   Reset histories, pending diagnostic readbacks and settling/scheduling state. The runtime entry points,
   capability parameter table and device owner remain session-owned CPU state.
3. Existing `ParkNr*` helpers stamp untracked present serials. Mark *all* parked untracked entries, including
   earlier ones, `requiresPresentFence`; later untracked parks during the transition inherit the same gate. `TickNrRetired` retains these until the private submission proof,
   the existing 32-evaluation count and the existing later present fence all pass. The present-fence registry
   must match the retained legacy queue's canonical COM identity; a same-adapter fence on another queue cannot
   authorize collection. Missing/multiple-queue/
   failed fences have **no count-only fallback** for migration entries. The guard is inside the collector,
   so guide-capture calls to that collector cannot bypass it. Shutdown retains the entire pending transition;
   it cannot bypass these gates. Device loss retains/abandons, never destroys the generation.
4. Once no migration entry remains, arm sticky tracking and let normal tracked creation build fresh model,
   textures and helper passes. The new composition may be allocated while draining, but records no NR GPU
   commands before admission. Tracker/creation failures remain normal refusals; the code never reverts to
   untracked recording. There is no repeated generation retirement if settings change while draining.

Transport-owned resources stay under their existing transport lifetime, rather than being reclassified:
backbuffer/HDR conversion, zero/synthesized guides, captured guide rings and capture/timing machinery. They
can still be used to pass the untouched frame through during the NR pause. All old NR reads of them finish
before migration completes; tracked NR thereafter borrows them through the existing contracts. Pre-upscale
scratch cannot belong to a migratable history: its admission would already have marked the history external.

`g_compose` is now shared with active callers. Acquisition is under `g_nrMutex`, and Dispatch refuses a detached
instance before recording. This covers a caller delayed between obtaining the composition and entering
Dispatch; replacing a bare unique owner would otherwise permit a null/stale dereference. Retirement owns a
heap-held shared reference through a raw pointer: abandoning the retirement entry on device loss also avoids
running its GPU destructor during static vector teardown. A caller's final reference may delay destruction
past fence collection, never bring it forward.

Lock order remains pre-scope -> NR -> diagnostic session/status and the existing present-fence leaf lock.
Submission/cancellation notification acquires NR only after queue calls/abandon return; no queue call is made
under a new lock. The transition does not call shutdown, take the pre-scope mutex or wait on the CPU for GPU progress.
It preserves the collector's release order, including enlargement destruction after vector edits.

### Validation boundary

`tests/nr-untracked-retire` runs the production admission/provenance functions with host fakes, plus the real
retirement slices under ASan/UBSan: default no tracking, pending CPU list, cancellation, delayed GPU fence,
removed device, retirement delay, rollback during migration, invalid lease, tracker refusal, sticky tracking,
individual/adaptive opt-ins and late mixed-queue refusal. Source guards inventory every `NrState` resource,
check composition acquisition/stale-generation refusal and submission/cancellation ordering. The existing
scene-stage, pre-upscale, multipass, present-fence and internal-linkage suites cover adjacent contracts.

Required runtime follow-up: native Windows and Proton on the route matrix; one -> two -> ten -> one passes,
individual/adaptive enable, rapid setting changes, outstanding asynchronous present work, deliberate signal/
device loss and NR shutdown during draining. Confirm requested/completed counts, history restart and unchanged
default pixels/commands. No visual, performance or GPU correctness claim follows from the host tests.

## Live opt-in from the upscaler's output (2026-11-20)

Host-tested only. Not built, not run on a GPU, not run in a game.

### What was asked, and what was not done

The owner asked on 2026-11-20 that raising `Passes` in the menu take effect without restarting the game. The
session that prompted it (native Windows, RTX 3060, Dying Light: The Beast, release `d3bd1ea6`) started at one pass
after the upscaler and answered "Restart required: prior untracked NR is not confined to private present lists".

The migration above cannot serve that route: the recordings are in the game's own command list, the module never
sees them submitted, and nothing can say afterwards that they ran. Two ways to get that evidence were weighed and
not taken:

- **Track every recording from the first frame.** This is real evidence, but it enrolls the default path. The
  tracker detours `ExecuteCommandLists` and command-list `Reset` for the process and takes one mutex in both, on
  every thread that resets or submits; the batch carrying NR holds a second mutex across the driver's
  `ExecuteCommandLists` and an added `Signal`. At one pass it would also bring the chain's admission, which can
  skip frames. None of that exists at `Passes=1` today (unless GPU timing or another tracked diagnostic is on).
- **The untracked path's own rule** (32 ticks and a completed present fence). Rejected for a migration on
  2026-11-19, and still rejected: it is not a submission argument for a list the game owns.

### The rule

The old generation is not proven finished, so it is not freed. It is detached, taken out of the retirement list
and kept until the process ends; tracking starts on fresh objects.

It applies only when all of these hold, under `g_nrMutex`, in `TrackNrRecording`:

- the request is an opt-in (`Passes` or `RRPasses` above one, individual settings, adaptive cadence) and not
  mandatory `Dx11SceneStage`;
- every untracked recording of the session was made by `EvaluateAfterUpscale` (after the upscaler or after Ray
  Reconstruction), and the recording that carries the request is one too. Each route names itself:
  `DlssNrFrameInfo::AfterUpscaleRecording` is set in that one function, after the copy it captures for the present,
  as `PrivatePresentRecording` is set in `RunPresentPass`. A recording that names neither is `Other`;
- the driver proxy is off (it runs one pass whatever is asked, and its feature is outside NR retirement);
- no other recording scope holds the lease;
- a second generation fits beside the first (below);
- the tracker accepts this command list (a probe `Submission::Track`, before anything is detached);
- no generation was left behind earlier in the session.

When the route or the history disqualifies it, the status and behaviour are the ones from before. When one of the
other conditions fails, one pass keeps running untracked and the status says which: "Not enough video memory to
add passes without a restart: ...", "Passes cannot be added here: this runtime's command lists cannot be
followed. ...", the proxy's line, or "transition deferred". The adapter and the tracker are asked again after 128
NR ticks, not on every frame the request stands.

### The switch

1. `SecondGenerationFits` asks the adapter (`VideoMemory`) and applies `Chain::AdmitBesideRetained`: the admission
   `Dispatch` applies to a new chain generation, with the usage that still counts the first generation, the
   surfaces `Dispatch` counts (`Chain::GenerationSurfaceBytes`, from the live generation's sizes) and the larger of
   the measured model figure and the model's own count. It is never more permissive than the admission the tracked
   path asks next, so passing here means the fresh generation is admitted unless the adapter's usage grew in
   between.
2. `RetainUntrackedGeneration` runs the migration's detach (`ParkUntrackedGeneration`, the same inventory) without
   its fence gate, then `LeaveUntrackedBehind` moves every untracked entry out of `g_nrRetired` into a list nothing
   else reads. That includes what earlier rebuilds parked: the tracked path would otherwise step their countdown
   faster than the path that parked them. Present-guide clones stay in `g_nrRetired` on their transport rule,
   because their bytes are in that ring's budget; tracked entries stay.
3. In the same call: `g_legacyRecorded = false`, `g_tracked = true`, status "Switching to tracked passes; the
   previous model stays in video memory until the game closes", and the call returns false. It records nothing:
   the `Dispatch` running it belongs to the composition object just detached.
4. The next call finds no composition, builds one, and is an ordinary tracked recording: 500 ms of settling, the
   first model created and waited for across a present, extra passes one per stable present.

Arming in the detaching call, not the next one, is what makes it once per session: there is no state in which the
generation is gone and a return to one pass could resume untracked recording. `s_generationRetained` refuses a
second one independently.

Nothing frees what was left behind. The collector, both parking caps (the chain's and the guide capture's),
`MakeRoomForGeneration` and `Shutdown` walk `g_nrRetired` only, so they are unchanged and cannot reach it. The
entries are raw owners, so static teardown destroys no GPU object.

### What the old lists can still reference

Audited on 2026-11-20 for the after-upscale and after-RR routes, from every command `Dispatch` and
`EvaluateAfterUpscale` record.

| Object | Lifetime rule | After the switch |
|---|---|---|
| Model feature, pass features; `colorCopy`, `output`, `hdrCopy`, `colorSmall`, `outputNative`, `passPing`, the alternate-format set, `heldColor`; `depthClone`, `motionClone`, `depthConstant`, `depthMatched`, `motionMatched`; `unpackedProxy`, `unpackedAnswer`; meter and calibration textures and readbacks | NR retirement (`ParkNr*`, by `g_tracked`) | left behind; the new generation allocates its own |
| Composition object (descriptor heaps, mapped constants), both scalers, stabilizer, UI mask, cadence store, guide match, periphery, enlarger bundle, detail-measure pass and copies, pulse pass, motion probe | NR retirement | left behind; rebuilt on demand |
| GPU timing slots, kernel profile slots, exposure scan slots and adopted buffers, comparison capture, replay recorder, guide clones captured for the present | each slot's own `Submission` usage, in every mode | unchanged; a slot is reused when its own usage says so |
| Capture readbacks (`g_capture`) | their own frame count, in every mode | unchanged |
| The game's output, depth, motion and exposure | the game's | unchanged |
| The driver proxy's feature | its own size check, in every mode | not used at the switch (the proxy must be off) |

Nothing whose lifetime follows `g_tracked` is shared between the two generations. Three objects that do follow it
are not in the inventory, and each is excluded by the route rule instead: the before-upscale scratch exists only
after a recording of that scope, which makes the history `Other`; the present pass's backbuffer copy and the
bridge's surfaces are read by NR only in present-route recordings, which make it private or `Other`. The forwarder
entry points, the capability parameter block and the device owner are session-owned CPU state.

### What it costs

- **Video memory.** One model instance and its surfaces stay allocated until the game closes, at most once per
  session. The INFO line at the switch gives the model's own count (when the loader reports one) and the adapter's
  size for the surfaces; helper passes are in neither. An instance measured 578 to 767 MiB at a 2292x960 working
  size on an RTX 4090 ([model-vram-stats](model-vram-stats.md)); it is not measured at 864x486. That memory counts
  against the budget that admits the extra passes, so fewer may fit than after a restart. The menu says so under
  the chain status for the rest of the session.
- **A pause.** About the chain's settling and creation ramp without NR, then pass one, then the rest.
- **The model's own count** shown in the menu includes the instance left behind from then on.
- **One pass at `Passes=1` gains nothing** until the request: the provenance flags, one bool on the frame, and no
  tracker call, lock, wait, allocation, adapter query or log line (`tests/nr-untracked-retire` counts them over the
  production body).

### Residual risk, stated

- If the adapter's usage grows between the check and the fresh generation's own admission, that admission refuses
  with nothing to make room from and latches the failure until Retry. One pass does not come back in that session.
- The tracker's probe is a real `Track`: on a runtime it refuses, the queue hook may stay installed and inert.
  That happens only after the request, never at `Passes=1` before it.
- The old lists may still be unsubmitted when tracked recording starts. Their objects are alive and private to
  them; what they write is the game's output of their own frame, ordered by the game, as on any frame.
- Tracking stays on for the session, with the admission rules above, even after returning to one pass.

### Validation boundary

`tests/nr-untracked-retire` runs the production `TrackNrRecording` for every case above (20 mutants, among them
enrolling `Passes=1`, skipping the budget or tracker check, allowing the bridge or a private history, retaining
twice, not arming with the detach), the production `LeaveUntrackedBehind`, collector and both caps (3 mutants,
among them leaving the generation where the collector frees it), and source guards on the detach, the route mark
and the one-pass branch. `tests/nr-multipass` holds the budget rule, `tests/nr-model-stats` the log line,
`tests/nr-before-upscale` the route mark on the production entry points.

Not shown by any of it. A run on native Windows (RTX 3060, Dying Light: The Beast, started at `Passes=1` after the
upscaler, GPU timing off and on) must record: no "Restart required" on raising to 2, then 10, then back to 1;
the INFO line with the retained bytes; `requested=N completed=N`, or the "Additional pass refused" status with its
admission line; `model_failed=0`, no device removal, no DRED or crash note; the adapter's usage before and after;
the budget refusal leaving one pass running (forced on a full adapter); a resolution change and a pause after the
switch; and at `Passes=1` before any change, the same coverage line and presents per window as `4a740a36`.
