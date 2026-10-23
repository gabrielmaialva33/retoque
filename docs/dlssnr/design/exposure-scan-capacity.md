# DLSS-NR: exposure scan capacity, and what makes a candidate safe to hold

Design before implementation (2026-10-23). It covers `retoque/dlssnr/DlssNr_ExposureScan.cpp` and its two
call sites in upstream files: the resource creation hooks in `OptiScaler/hooks/D3D12_Hooks.cpp` and the
release hook in `OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp`. Nothing here changes what the scan does when it is
not wanted (`WhitePointSource` is not 2 and `ScanExposure` is off), or when `[DlssNr] Enabled` is off.

## 1. The problem

The scan has to find one buffer among everything the game creates that is shaped like a number. Two rules
make it miss the one it is looking for:

- **A full list refuses newcomers.** `Adopt` stops at 64 candidates (`DlssNr_ExposureScan.cpp:351-362`) and
  says so once. Whatever is created after that is never looked at. Buffer-heavy engines fill the list at load
  with things that are not an exposure: janblade measured Cyberpunk 2077's exposure arriving as candidate
  61-63, RDR2's 1x1 and Doom Eternal's 2x2 exposure images shut out by 64 earlier buffers, and The Witcher 3
  creating 387 buffers of the scan's shape in under half a second, the first 256 holding only integers or
  zero (their commits `c0d01226`, `0bec0a9a`; their measurements, not repeated here).
- **A feature release empties the list for good.** `NVSDK_NGX_D3D12_ReleaseFeature` calls
  `ReleaseTrackedResources` first (`NVNGX_DLSS_Dx12.cpp:867`), which releases and forgets every candidate
  (`DlssNr_ExposureScan.cpp:1081-1094`). Its comment ends "The scan re-adopts candidates next frame". It does
  not: `Adopt` is reached only from `NoteResource` (a resource being created) and `NoteUav` (a view being
  created), so a buffer that already exists is never seen again. A game that keeps its exposure buffer across
  a DLSS quality change loses it from the scan at the change. Cyberpunk is the other shape: janblade measured
  it re-creating its exposure buffers 0.25 s *before* it releases the upscaler feature (`9c764ff6`), so the
  new ones are either refused by the full list or wiped by the release that follows.

Reading the code for this found two more defects that any change to the list makes worse:

- **Readings are credited by list position.** Each sweep copies candidate `i` to offset `i * 512` of a
  readback slot, and the slot is read four sweeps later against whoever is at position `i` *then*
  (`:546-606`). After the wipe, or any change to the list, a new candidate is credited with its
  predecessor's value. Two different buffers' values in one range is exactly what sets `moves` and declares
  a find.
- **A reading needs no proof that the GPU wrote it.** The slot is read because four sweeps have passed
  ("Retired by now", `:544-552`). `retoque/dlssnr/README.md` ("Never free under the GPU") and
  `submission-tracking.md` are explicit that elapsed frames are not completion evidence. A list that was
  never executed leaves the slot holding whatever it held, and that is credited as a reading.

## 2. What the wipe prevents

The references exist because the scan copies the candidates every sweep: without one, the game could destroy
a resource between the scan recording a copy and the GPU running it. They were dropped twice, for reasons
written into the code:

1. `70f17250` (2026-09-03, Cyberpunk 2077): the scan AddRef'd every candidate and never released any. A
   Streamline/DLSS-D resource that passed the filter outlived its owner's teardown; the device was removed
   with "a lock on a freed object in nvwgf2umx" (native Windows, NVIDIA's D3D12 driver). The commit's own
   reading is that the owner freed the placed heap under a resource our reference kept alive. The fix
   released every reference at the top of `ReleaseFeature`, before the feature frees anything, and names its
   residual: a heap freed outside that path is not covered.
2. `ef14d066` (2026-09-29, ported from mrcgibb9876-hash `ba6fcc84`, their measurement): Cyberpunk with DLSS
   Frame Generation removed the device within seconds while the scan held 64 references to small buffers
   "Streamline and the game create, recycle and free every few frames" -- with the scan *not wanted*, so
   nothing was being copied. The fix holds references only while the scan is wanted. How a reference that
   is never used removes a device is not established. The plausible reading, consistent with (1), is that the
   eventual release destroyed a placed resource after its owner had freed the heap under it.

So the wipe gives three properties, and these are what a replacement has to keep:

- **P1.** No scan reference keeps a resource alive indefinitely after its owner is done with it.
- **P2.** The scan does not record a copy of a resource after the teardown that freed it.
- **P3.** The scan's release of a resource happens while the memory under it still exists, so the release
  only decrements a count and destruction happens in the owner's own order.

It gives them in the crudest way: at the one teardown point it can see, everything goes. It does not cover
a heap the game frees at any other moment (level streaming; the recycling in (2)): until the next feature
release the scan keeps holding such a resource and keeps copying it every sweep.

**What this change gives instead, as revised after Codex's adversarial review (2026-10-23).** The review found
that the first version claimed P1-P3 as guarantees the code did not give. As revised, they read:

- **P1, with its exception.** A candidate is released within a few frames of its owner letting go, once the
  scan's copies of it are proven done, and at once when the scan is turned off and nothing of it is in flight.
  One whose copies are never proven done is retained, within a budget of 256 (3.3).
- **P2, narrowed.** After the probe that finds the owner gone, nothing copies the candidate again. One copy can
  still be recorded in the window between the probe and the recording if another thread drops the last
  reference there (5); the scan's own reference keeps the object alive through it. Neither the probe nor anything
  else here certifies that a candidate is not aliased, is resident, or is in the state the copy assumes (3.4).
- **P3, at the COM level.** The scan's release of a placed candidate runs before the release of the heap the
  scan holds under it, so it cannot run on a heap the scan let be destroyed. The last reference may be the
  scan's, so destruction can run in the scan rather than in the owner. Driver state that does not follow COM
  references is not covered (5).
- **Across a feature release**, only a committed candidate on a live device is carried; everything else goes as
  the wipe did (5).

## 3. Candidate lifetime

### 3.1 Identity and origin

Every adoption gets an **id**: a serial number that is never reused, not even for a new resource at the
address of a destroyed one. The id is what the menu and the log show as "candidate N", and what readings are
credited to.

Every candidate has an **origin**, fixed at adoption:

- **committed**: seen by the `CreateCommittedResource` hook. The resource owns its memory: a reference to it
  is a reference to the memory.
- **placed**: seen by the `CreatePlacedResource` hook, which also receives the heap. The scan takes a
  reference on the **heap** together with the resource, and releases the heap only after the resource. So a
  heap cannot be destroyed while the scan still holds a resource placed in it, and the scan's release of the
  resource never runs on freed memory (P3), whatever order the owner frees things in. Whether the runtime
  already keeps a heap alive for its placed resources is not established here; the removal in `70f17250`
  says the scan must not rely on it. The cost is memory: a heap the game has let go stays allocated until
  the scan lets go of the last candidate in it (section 3.3: normally a few frames).
- **view**: adopted from `NoteUav` (a view creation, seen only while the HUD fix's resource tracker runs) for
  a resource whose creation the scan did not see: `CreateCommittedResource1/2/3`, `CreatePlacedResource1/2`
  and reserved resources are not hooked. The heap is unknown, so P3 cannot be given by pinning, and these
  keep the wipe exactly (section 5).

### 3.2 States

| State | Holds a reference | Copied | Can become the scan's value |
|---|---|---|---|
| **active** (up to 64) | yes | only when certified this sweep (3.4) | yes, from certified readings |
| **waiting** (up to 192) | yes | never | yes, from readings it got while it was active |
| **retiring** | yes, until its copies are proven finished | never | never |
| released | no | never | never |

Active is the set the sweep copies; its size is the readback slot's 64 offsets, unchanged. Waiting is where
a candidate goes when active is full, or when it is moved aside for one that has not been tried (section 4).
A candidate is in exactly one state, and one resource is one candidate: a resource already held in any state
is not adopted again.

### 3.3 Retirement

A candidate stops being copied and moves to retiring when:

- **the game let it go**: before every sweep the scan probes each held candidate's reference count
  (`AddRef`, then `Release`'s return value). If the scan's own reference is the only one left, the owner has
  released it. Nothing after that point copies it (P2 as narrowed above, for every way a resource ends, not
  only a feature release).
- **it is false** and the list is under pressure (section 4).
- **it is dropped** from a full waiting list (section 4).
- **a feature is being released and it may not be carried** (placed, view, removed device: section 5).
- **the scan is not wanted any more, or NR is off** (section 5).

A retiring candidate keeps its reference (and its heap's) until the submission tracker says every recording
that copied it has **settled** (`Submission::Settled` on the candidate's own usage, which every copy of it
was tracked into). A candidate that was never copied has nothing to wait for and is released at once. Because
the usage gains a recording every sweep, the ones retirement no longer waits for are dropped before each new one
(`Submission::PruneFinished`). That drops an epoch only when `Settled` itself would no longer wait for it -- never
one that is merely completed, whatever the usage's free space (review E-01: the first version used `Admit`'s
pruning, which cuts completed epochs to keep eight places free, and so could forget a list that is executed
again). A full usage refuses the candidate's next copy instead. With readback slots reused only after
settlement (3.4), a candidate's usage holds at most a few unfinished recordings, so it does not fill.

**The copy budget** (review E-05). A candidate's *first* copy is admitted only while fewer than 256 held
candidates may still have GPU work the scan recorded: copied and active, copied and waiting with copies not yet
settled, and retiring. Retiring candidates are made only of those, so the retiring set can never exceed 256,
however many owners let go at once. The first version only paused promotions once 256 were already retiring,
which a batch of let-gos could overshoot. The budget counts references, not bytes: a placed candidate also
holds its heap (3.1).

That is **P1** with its exception stated: a resource whose owner let go is released as soon as the GPU is
proven done with the scan's copies -- normally a few frames -- and one whose copies are never proven done is
retained, within the budget, for as long as that lasts.

A candidate whose copies never settle (a failed fence, a list the game never submits) is **retained**, as the
module's rule for unproven GPU work requires; it is never copied again, its heap stays pinned so the eventual
release is still safe, and the budget above stops more of them from accumulating.

The one exception is a **removed device**. Its fences never answer (`GetCompletedValue` reads `UINT64_MAX`), so
nothing on it would ever settle, and waiting would hold the removed device's objects, with the heaps under them,
for the rest of the session -- where the wipe used to let them go at the next feature release. Nothing recorded on
a removed device can run again, and releasing its objects is what recovering from a removal is. So a retiring
candidate whose own device (`GetDevice`, then `GetDeviceRemovedReason`) reports removed is released without
waiting.

### 3.4 Certification

Three separate certificates, each with one meaning:

- **Certified to copy, this sweep**: active; probed in this same `Tick`, under the scan's lock, with a holder
  other than the scan; and tracked: `Submission::Track` accepted this command list both into the
  readback slot's usage and into the candidate's own. Anything that fails one of the three is skipped this
  sweep, and its offset in the slot records no id. **What this certifies is narrow** (review E-03): the COM
  object is alive and someone besides the scan holds it at the probe, and the copy's lifetime is tracked. It
  does not certify that the resource is still the one active in its memory (aliasing), that it is resident
  (`Evict`), that it is in `UNORDERED_ACCESS` (an assumption the scan has always made), or that the holder is
  still using it; and another thread can release its reference between the probe and the copy (the scan's
  reference keeps the object, not the game's claim on it). Those were the scan's assumptions before this change
  and still are; what limits them is that only a committed candidate is carried past a feature release (5).
- **Certified reading**: a value comes from a readback slot whose recording `Submission::Completed`, and is
  credited only to the candidate whose id that slot recorded at that offset when the copy was recorded. A
  value for an id that is retiring or gone is dropped. Readings no longer depend on list position or on
  elapsed sweeps.
- **Certified to use** (`BestValue`, the Found verdict, the white point): not retiring, and `moves` computed
  from certified readings only.

A slot is read once its recording completed, and **rewritten only once that recording has settled** -- Ready
(completed and Reset), cancelled, or completed and aged out of the tracker's replay window -- not merely
completed (review E-04: a list executed again after its slot was reused wrote the old candidate's value into
the new recording's offset, and it was credited to the newcomer). A slot whose recording does not complete --
the GPU behind, a Signal that failed, a list never submitted, a removed device -- is never read and never
reused, so the sweep that would write it is skipped. When every slot is waiting the scan says so (section 7)
rather than reading anything. In a game that never resets its old lists this slows the sweep to about four per
replay window (128 renderer ticks); measured by `fresh-lists`.

What remains is the tracker's own window, which this inherits and does not narrow: outside strict mode a
completed, unsealed recording counts as finished once 128 ticks old, for every user of `Settled`. A list the game
executes again later than that, without a Reset, is outside the contract of the whole module, not only of the
scan; the first replay the tracker sees latches strict mode, after which only Ready counts.

When `Track` refuses the command list (an unsupported list type, queue hooks unavailable, a runtime the
tracker does not admit: `submission-tracking.md`), the scan copies nothing on it and says why once, as the
pulse metric does. Before this change it would have copied and read on frame counts alone.

## 4. A full list

Admission, in `NoteResource` and `NoteUav` (any thread, under the scan's lock):

1. Active has room: the newcomer becomes active. It is not copied until a sweep has probed it.
2. Active is full: it waits, at the back of the waiting list.
3. Waiting is full (192): the oldest waiting candidate that was never copied is released (no GPU use to wait
   for) and the newcomer takes its place -- a late resource is the likelier exposure (eye adaptation is set
   up with the post-processing), and one that has been waiting longest is the one most likely to be gone.
   If every waiting candidate has been copied before, the newcomer is refused, logged once per release
   generation.

In every sweep, after the probe and before recording, while something is waiting:

1. **Fill**: free active positions are taken by the oldest waiting candidates first.
2. **False goes first**: an active candidate with at least 60 certified readings, none of them plausible (in
   `(1e-6, 1e4)` and finite), is retired. Zero, a counter, a sentinel and an integer read as a float (65536 is
   `9.18e-41`) all qualify; an exposure is written every frame from the start, so 60 readings without one
   plausible value is not one. Non-finite values now count as implausible readings, where they used to be
   skipped, so a buffer that only holds NaN can be judged too.
3. **Then the quiet**: if nothing false is left and a waiting candidate has never been tried (fewer than 1800
   readings: the existing patience), the active candidate that has watched longest past 1800 readings without
   moving is moved to the back of the waiting list. It keeps its reference and its readings, so it can come
   back. A candidate that moves is never moved aside or retired by this policy.

Nothing is retired as false while nothing is waiting: with room in the list the scan keeps what it always
kept. The verdict is **Barren** only when nothing moves and every held candidate, active or waiting, has been
read 1800 times; while anything untried waits, it is Watching, and the sweep keeps its discovery cadence.

## 5. ReleaseFeature and recreation

`ReleaseTrackedResources` stays the call at the top of `NVSDK_NGX_D3D12_ReleaseFeature`, and becomes a
**boundary** rather than a wipe:

- **view** and **placed** candidates, in any state, are released there and then, as the wipe did, and so is any
  candidate whose device was removed (section 3.3). For a view candidate the heap is unknown; for a placed one
  the scan holds the heap, but holding a heap keeps memory allocated, not the resource's claim on it, and a
  feature release is when a game reorganises its allocations (review E-03). Within an era a placed candidate is
  still watched, with its heap pinned, as before this change.
- **committed** candidates on a live device are kept, with their readings. A committed resource owns its
  memory and cannot be aliased. None is copied again until a sweep after the boundary has probed it, and every
  sweep probes before it records (3.4). One the teardown freed is found let go at the next sweep, never copied
  after it, and released once its earlier copies settle. One the game still holds stays watched.
- **With the scan off, or NR off,** nothing is kept (review E-02): every candidate is let go there, the ones
  with unsettled copies kept only until those settle. The same happens at the first frame of the pass after the
  scan is turned off, without waiting for a release; with nothing held, that check is one relaxed load.
- The once-per-generation log flags (the full list, the barren cadence) are reset, as the wipe did.

Recreation before a release then needs no special window **if the buffers are committed**. In Cyberpunk's
order: the new buffers are created while the list is full and wait; the old feature is released; the game lets
go of its old buffers, and the next sweep finds them let go and retires them; their active positions are filled
from the waiting list; the new exposure is read and found. A game that keeps a committed exposure across the
release keeps it in the scan too. If Cyberpunk's exposure buffers are placed, the release still drops them, as
the wipe did, and the scan finds the exposure only if it is created again later; which it is, is for the game
run to say (the adoption line names the origin).

janblade's fork kept candidates adopted in the last 2 s before a release (`9c764ff6`, `b0c63dd7`). That keeps
anything fresh, including a resource of the very feature being released (a feature made and released inside
2 s, such as the thumbnail features `ReleaseFeature` already special-cases), and drops a game's long-lived
exposure. Probing after the teardown keeps exactly what still has an owner, whenever it was made.

### What this does not guarantee

- **Aliasing, residency, state, ownership** (review E-03). Within an era the scan copies a placed candidate
  that a game may have aliased (another resource activated over the same memory) or evicted, and any candidate
  in a state other than `UNORDERED_ACCESS`; this is not a wrong reading but GPU access without the activation
  or residency it needs. The base scan had the same exposure for as long as it held a candidate; this change
  does not narrow it within an era, and no longer extends it across a release for placed candidates. A
  committed candidate can be evicted too, and is carried; residency is not certified.
- **The probe-to-copy window.** The probe and the recording are under the scan's lock; the game's Release is
  not. A reference the game drops between them leaves one copy recorded of a resource the game had just let
  go. The scan's own reference keeps the object (and a placed one's heap) alive through that copy.
- **One candidate, one interface.** Candidates are deduplicated by pointer. A resource reached through two
  distinct interface pointers or proxies would be held twice, and the probe's fixed "minus one" would then
  see the scan's second reference as another holder. Nothing seen here does that; it is not ruled out.
- **Reference-count-driven pools.** Software that decides a pooled resource is free by its reference count
  sees the scan's reference as a user. Nothing found here does that, and nothing here rules it out.
- **Driver-internal objects.** The protection is for objects whose lifetime follows their COM reference
  counts. If the nvwgf2umx removal in `70f17250` came from driver state that is freed at a feature teardown
  whatever the counts say, pinning does not reach it; what does is that nothing the teardown let go of is
  ever copied after it, which is the action that removed the device then.
- **The mechanism of `ef14d066`.** Unknown. The scan still adopts nothing unless it is wanted, and since the
  review lets go of what it holds as soon as it is not wanted (5); what changes while it is wanted is that a
  recycled buffer is released within a few frames of its owner letting go, instead of at the next feature
  release, and that a committed candidate is held across releases.
- **Replay later than the tracker's window** (3.4). Within it, a replayed recording keeps its slot unused and
  its candidates held. Later than it, without a Reset, is outside the module's contract.
- **NR switched off between releases.** With NR off no frame of the pass runs, so candidates are let go at the
  next feature release, not at once; until then they are held, uncopied.

## 6. Cost

When wanted: one `AddRef`/`Release` pair per held candidate per frame (up to 256), one `Submission::PruneFinished`
and one `Submission::Track` per copied candidate plus one `Track` per slot, and the copies as before (at most 64 a
sweep, the same throttles). The readback ring is unchanged (four 32 KB buffers).

List probes (review cost note): `Settled` refreshes orphan flags with an `AddRef`/`Release` pair per held list of
each epoch it visits. The first version's `PruneFinished` did too, once per copied candidate: 3,648 pairs in one
`Tick` with 64 candidates on kept lists (`cost`, the review's figure). `PruneFinished` no longer refreshes (an orphan
flag only lets an epoch finish sooner, and the slot checks refresh the same shared epochs), and with slots reused
only after settlement a usage stays small: `cost` measures at most 4 per `Tick`, on kept lists and on reset ones,
and holds it under 448. Retiring candidates add one `Settled` each per frame, bounded by the budget.

Not wanted: one relaxed load per frame while nothing is held. NR off: nothing new runs.

## 7. What is logged

- Each adoption, as now, with its id, shape, origin and where it went (active or waiting); individual event
  lines (adopted, let go, false, moved aside, dropped, refused) are written for the first 256 events of the
  session, after which only the summary below carries them, so a game that recycles buffers every few frames
  cannot flood the log.
- The existing ~300-frame readout gains one summary line: active, waiting, retiring, and since the last
  summary how many were adopted, let go by the game, retired as false, moved aside, dropped, refused and
  released.
- At each boundary: how many committed candidates were kept to be re-checked, and how many placed, view or
  removed-device ones were released; with the scan off, how many were let go and how many wait for the GPU.
- Once: a command list the tracker refuses (with its reason); the first time every readback slot is waiting
  on an unconfirmed recording; the first time the copy budget stops a first copy.
- The headline's Watching line counts the waiting candidates too.

## 8. Tests

`tests/nr-exposure-scan` compiles the production `DlssNr_ExposureScan.cpp` against fake D3D12 objects and
the production submission model (`DlssNr_SubmissionModel.h`), under ASan and UBSan. The fakes count
references, keep destroyed objects' memory so a use after destruction is caught rather than undefined, and
record a placed resource destroyed after its heap as a violation. A fake list records copies and barriers;
executing it writes each source's value into the readback memory and notifies the model, and the test
decides when (and whether) each fence completes. Cases:

- **Full list, then a late real candidate**: 64 zero buffers, then a moving exposure. It waits, the zeros are
  retired as false after 60 certified readings, it is promoted, read and found, under its own id.
- **Recreation before release**: a full list of the game's old buffers, a new set (with the exposure)
  created, the feature released, the old set let go. The old ones are never copied after they were let go
  and are released only after their copies settled; the new exposure is found.
- **A long-lived exposure across a release** is still watched after it, with its readings.
- **False candidate and attribution**: a newcomer at a position a different buffer had does not receive that
  buffer's value; an integer buffer is never used and is the first to give way.
- **Quiet candidates**: 64 plausible buffers that never move are not retired as false; once watched past the
  patience one is moved aside, still held, for the exposure waiting behind them, and the verdict stays Watching
  until then.
- **Waiting overflow**: with 64 read and 192 waiting, a newcomer takes the place of the oldest never-copied
  waiting candidate, and only that one is let go.
- **Lists never reset**: a new list every frame, none reset; still copied in every 128-frame window (four
  sweeps each), and found.
- **Review cases** (2026-10-23): `replay-prune` (a recording executed again keeps its candidate held, E-01),
  `prune-model` (the model's `PruneFinished` keeps 57 completed, unsealed recordings that `Admit`'s pruning would
  cut), `replay-slot` (a slot is not rewritten while its last recording can run again, E-04), `off-after-on`
  (frames with the scan off, a release, on again, and NR off with only a release: nothing held, E-02),
  `placed-boundary` (E-03), `budget` (built with a budget of 8: first copies, retained let-gos, E-05), `cost`
  (list probes per `Tick`, printed and bounded), and `boundary-probe` (no frame of the pass: a let-go committed
  candidate is released at the next release).
- **Failed fence**: a recording whose Signal fails, or whose fence never completes, is never read, its slot
  is never rewritten, and a candidate copied in it is not released. On a removed device the same holds for the
  reading, and the candidate is released once the game lets it go, as it is at a boundary.
- **Freed, address reused**: a resource let go, released after settlement and destroyed, and a new one
  created at the same address: a new id, and a late completion of the old copy is not credited to it.
- **Placed heap**: the game releases a placed candidate and its heap; nothing is destroyed out of order and
  nothing is copied after the release.
- **View candidates** are released at the boundary, as before.
- **Inert**: NR off holds and records nothing; the scan not wanted holds nothing; a refused `Track` copies
  nothing.
- Source guards: the placed hook passes its heap, `ReleaseFeature` still calls the boundary first, the sweep
  probes before it records.

The cases run first against the code before this change and are expected to fail there, all but the inert
ones. Not covered by a case: the refusal when every waiting candidate has been copied before, and the retiring
bound (256) stopping promotions; both are reached only through hundreds of unsettled or moved-aside candidates. A green suite says the bookkeeping does what this note says against fakes. It says nothing about a
real driver, real reference counts under vkd3d-proton or Windows, or whether the scan finds Cyberpunk's
exposure: that needs the game, or the replay gates.
