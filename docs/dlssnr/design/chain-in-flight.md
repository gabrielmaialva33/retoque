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
  assume the GPU is less than about ten frames behind.

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
