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
  released it. Nothing after that point copies it (P2 for every way a resource ends, not only a feature
  release).
- **it is false** and the list is under pressure (section 4).
- **it is dropped** from a full waiting list (section 4).
- **its origin is view and a feature is being released** (section 5).

A retiring candidate keeps its reference (and its heap's) until the submission tracker says every recording
that copied it has **settled** (`Submission::Settled` on the candidate's own usage, which every copy of it
was tracked into). A candidate that was never copied has nothing to wait for and is released at once. The
retiring set is bounded at 256: above that nothing new is promoted to active (nothing new is copied, so
nothing new can need settling), until it drains.

That is **P1** in a stronger form than the wipe's: a resource whose owner let go is released as soon as the
GPU is proven done with the scan's copies -- normally a few frames -- instead of at the next feature
release, which in a game that never changes its settings is never.

A candidate whose copies never settle (a failed fence, a list the game never submits) is **retained**, as the
module's rule for unproven GPU work requires; it is never copied again, its heap stays pinned so the eventual
release is still safe, and the bound above stops more of them from accumulating.

### 3.4 Certification

Three separate certificates, each with one meaning:

- **Certified to copy, this sweep**: active; probed in this same `Tick`, under the scan's lock, with a holder
  other than the scan; and tracked: `Submission::Track` accepted this command list both into the
  readback slot's usage and into the candidate's own. Anything that fails one of the three is skipped this
  sweep, and its offset in the slot records no id.
- **Certified reading**: a value comes from a readback slot whose recording `Submission::Completed`, and is
  credited only to the candidate whose id that slot recorded at that offset when the copy was recorded. A
  value for an id that is retiring or gone is dropped. Readings no longer depend on list position or on
  elapsed sweeps.
- **Certified to use** (`BestValue`, the Found verdict, the white point): not retiring, and `moves` computed
  from certified readings only.

A slot is written only after its previous recording completed. A slot whose recording does not complete --
the GPU behind, a Signal that failed, a list never submitted, a removed device -- is never read and never
reused, so the sweep that would write it is skipped. When every slot is waiting the scan says so (section 7)
rather than reading anything.

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

- **view** candidates, in any state, are released there and then, exactly as before. The scan cannot pin
  their memory, so for them the old guarantee is kept by the old means (P1-P3 as in section 2).
- **committed** and **placed** candidates are kept, with their readings. None of them is copied again until a
  sweep after the boundary has probed it, and every sweep probes before it records (3.4). A candidate the
  teardown freed -- the documented Streamline/DLSS-D case -- is therefore found let go at the next sweep,
  never copied after the teardown (P2), and released once its earlier copies settle, with its heap still
  pinned (P3). A candidate the game still holds after the teardown stays watched.
- The once-per-generation log flags (the full list, the barren cadence) are reset, as the wipe did.

Recreation before a release then needs no special window. In Cyberpunk's order: the new buffers are created
while the list is full and wait; the old feature is released; the game lets go of its old buffers, and the
next sweep finds them let go and retires them; their active positions are filled from the waiting list; the
new exposure is read and found. A game that keeps its exposure across the release keeps it in the scan too.

janblade's fork kept candidates adopted in the last 2 s before a release (`9c764ff6`, `b0c63dd7`). That keeps
anything fresh, including a resource of the very feature being released (a feature made and released inside
2 s, such as the thumbnail features `ReleaseFeature` already special-cases), and drops a game's long-lived
exposure. Probing after the teardown keeps exactly what still has an owner, whenever it was made.

### What this does not guarantee

- **Aliasing inside a live heap.** An owner that reuses a placed resource's memory for something else while
  still holding the resource object makes the scan read someone else's bytes. The probe cannot see that; the
  result is a wrong reading, not a freed page, and it is the same exposure the scan has today.
- **Reference-count-driven pools.** Software that decides a pooled resource is free by its reference count
  sees the scan's reference as a user. Nothing found here does that, and nothing here rules it out.
- **Driver-internal objects.** The protection is for objects whose lifetime follows their COM reference
  counts. If the nvwgf2umx removal in `70f17250` came from driver state that is freed at a feature teardown
  whatever the counts say, pinning does not reach it; what does is that nothing the teardown let go of is
  ever copied after it, which is the action that removed the device then.
- **The mechanism of `ef14d066`.** Unknown. The scan still holds nothing unless it is wanted; what changes is
  that while it is wanted, a recycled buffer is released within a few frames of its owner letting go,
  instead of at the next feature release.
- **Replay.** A list the game executes again without a Reset re-runs old copies into a readback slot the
  scan may since have rewritten. The tracker latches strict mode when it sees one; a slot rewritten in
  between can then credit one stale value. Bounded and named, not handled.

## 6. Cost

When wanted: one `AddRef`/`Release` pair per held candidate per sweep (up to 256), one `Submission::Track`
per copied candidate plus one per slot, and the copies as before (at most 64 a sweep, the same throttles).
The readback ring is unchanged (four 32 KB buffers). Not wanted, or NR off: nothing new runs.

## 7. What is logged

- Each adoption, as now, with its id, shape, origin and where it went (active or waiting); individual event
  lines (adopted, let go, false, moved aside, dropped, refused) are written for the first 256 events of the
  session, after which only the summary below carries them, so a game that recycles buffers every few frames
  cannot flood the log.
- The existing ~300-frame readout gains one summary line: active, waiting, retiring, and since the last
  summary how many were adopted, let go by the game, retired as false, moved aside, dropped, refused and
  released.
- At each boundary: how many candidates were kept to be re-checked, and how many view candidates were
  released.
- Once: a command list the tracker refuses (with its reason); the first time every readback slot is waiting
  on an unconfirmed recording; the first time the retiring bound stops promotions.
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
- **Failed fence**: a recording whose Signal fails, or whose fence never completes, is never read, its slot
  is never rewritten, and a candidate copied in it is not released.
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
ones. A green suite says the bookkeeping does what this note says against fakes. It says nothing about a
real driver, real reference counts under vkd3d-proton or Windows, or whether the scan finds Cyberpunk's
exposure: that needs the game, or the replay gates.
