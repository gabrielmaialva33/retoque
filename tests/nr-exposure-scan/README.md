# Exposure scan: candidate lifetime and capacity (host)

```bash
python3 tests/nr-exposure-scan/run.py
python3 tests/run_all.py --only nr-exposure-scan -v
```

The production `retoque/dlssnr/DlssNr_ExposureScan.cpp` compiled with `g++` under ASan and UBSan against
`stubs/` (the D3D12 declarations it names, its two Config keys families, the log macros) and driven by
`cases.cpp` through its public API, the way the creation hooks, the NR pass and `ReleaseFeature` drive it. The
submission tracker underneath is the production model (`retoque/dlssnr/DlssNr_SubmissionModel.h`); the case
decides when each recording executes, whether its Signal succeeds and when its fence completes. One case per
process, because the scan's state is global. Design: `docs/dlssnr/design/exposure-scan-capacity.md`.

## The fakes

- Every resource and heap a case makes is kept until the process ends, destroyed or not, so a reference
  taken, a barrier or copy recorded, or a GPU read of a destroyed object is a recorded violation instead of
  undefined behaviour. Each fake knows how many references are the game's, so the scan's own are counted
  exactly.
- A placed resource does **not** keep its heap alive, and a placed resource destroyed after its heap is a
  violation: the worst case the design has to survive (the device removal in `70f17250`).
- A list records copies and refuses nothing; recording a barrier or copy of a resource the game let go of, or
  over a freed heap, is a violation. A list keeps its commands until Reset. Executing it notifies the model and
  hands the GPU a copy of its commands, which run (writing each source's bytes into the readback memory, and
  recording a read of a destroyed resource or freed heap as a violation) when the case completes that queue's
  fence. Executing a list again replays them. Nothing tells the scan the data is there unless the fence
  completes.

## Cases

| Case | What it holds |
|---|---|
| `late` | 64 zero buffers fill the list, then a moving exposure: it is held, the zeros give way as false, it is found |
| `recreate` | a new set created while the list is full, the feature released, the old set let go: the new exposure is found, every old buffer is released, none is copied after it was let go |
| `long-lived` | an exposure kept across a feature release stays held and found, its readings carried on |
| `attribution` | a newcomer is never credited with a value its own buffer did not hold: after a release, and after a late completion of a copy of a let-go candidate at the same position |
| `integer` | an integer buffer (65536 reads as `9.18e-41`) gives way first; 63 plausible quiet buffers are kept |
| `quiet` | 64 plausible buffers that never move: nothing found and Watching (not Barren) until one is past patience, then it is moved aside, still held, and the exposure behind them is found |
| `overflow` | 64 read and 192 waiting: a newcomer takes the place of the oldest never-copied waiting one, and only that one is let go |
| `fence-never`, `fence-signal`, `fence-removed` | a fence that never completes, a failed Signal, a removed device: no reading, no slot rewritten; a let-go candidate with unconfirmed copies retained, except on the removed device, where nothing can run again and it is released |
| `discarded` | a list reset without executing frees its slot unread; readings resume when lists run |
| `reuse` | let go, retained while its copy is unconfirmed, released after it, and a new resource at the same address adopted fresh and found on its own values |
| `placed` | the game releases a placed exposure and its heap: nothing copied after, nothing destroyed out of order, both released by the end |
| `view` | a candidate seen only through a view is released at the boundary, as before; a committed one is kept |
| `removed-boundary` | a committed candidate is kept at a boundary on a live device and released at one once its device was removed |
| `fresh-lists` | a new list every frame and none ever reset: still copied in every 128-frame window (four sweeps each, slots waiting for their recordings to settle) and found; prints the copies per window |
| `replay-prune` | review E-01: a quiet candidate copied into 58 kept lists, the first executed again, the rest reset, the candidate let go: held until the replay ran and its list was reset |
| `prune-model` | review E-01, the model's `PruneFinished` on its own: keeps 57 completed, unsealed recordings inside the replay window (the admission pruning cuts one), drops them once aged |
| `replay-slot` | review E-04: a slot is not rewritten while the recording that last wrote it can run again; a replay of it cannot credit its old value to a newcomer |
| `off-after-on` | review E-02: frames with the scan off let go of what it held before any release; a release with it off; on again; NR off with only a release |
| `placed-boundary` | review E-03: a placed candidate and its heap are released at the boundary; a committed one is kept |
| `boundary-probe` | with no frame of the pass, a committed candidate the game let go of is released at the boundary |
| `cost` | review cost note: list probes in one `Tick` with 64 candidates, on kept and on reset lists, printed and bounded (3,648 before the review, 4 after) |
| `budget` | review E-05, built with a copy budget of 8: at most 8 first copies, none while the budget is held by unconfirmed copies, retained let-gos within it, copying resumes once confirmed |
| `inert-off`, `inert-unwanted` | NR off, or the scan not the white point's source: no reference, no copy (and with NR off nothing examined) |
| `untracked` | a list the tracker refuses gets no copies, and the status says why |

Source guards then check the call sites and the review fixes (the boundary carries only committed candidates and
lets everything go with the scan off, a Tick with the scan off lets go, a slot is reused only after its recording
settled, `PruneFinished` drops finished epochs only and the suite's stub of it is the production rule). Before
that, they check the placed creation hook passes its heap, `ReleaseFeature` calls the
boundary once and before the feature is torn down, each `Tick` probes before it sweeps, the sweep tracks the
slot and every candidate before recording, readings come only from completed recordings and never from elapsed
sweeps, and the boundary releases view candidates without forgetting the rest.

The review cases (`b0f73a5e`) failed against `0c527c43`, the first version of this change: 6 of them (`budget`,
`cost`, `off-after-on`, `placed-boundary`, `replay-prune`, `replay-slot`); `prune-model` and `boundary-probe` were
added with the fixes, and each fix was mutated back and caught (E-01 model rule by `prune-model`, E-04 by
`replay-slot`, E-02 by `off-after-on`, E-05 by `budget`, E-03 by `placed-boundary`, the boundary probe by
`boundary-probe`).

Against the code before this change (`acdba2af`) every case of the first fifteen fails except the two inert ones,
and so do the guards that name the new structure (`quiet`, `overflow`, `removed-boundary` and `fresh-lists` were added
after the implementation; `fresh-lists` failed against its first version, which tracked without pruning).
Five mutations of the new scan were each caught: a probe that never sees a let-go (4 cases), no heap reference
(`placed`), crediting by position instead of id (`attribution`), reading without completion (4 cases), and
releasing without settlement (5 cases), and keeping candidates on a removed device at the boundary
(`removed-boundary`). Not covered: the refusal when every waiting candidate has been copied
before, and the retiring bound stopping promotions.

## What it does not prove

Nothing here runs D3D12, a driver or a GPU. Real reference counts under vkd3d-proton or Windows, whether a
runtime keeps a placed resource's heap alive, Streamline's or NGX's own lifetime rules, and whether the scan
finds any particular game's exposure are all outside it. The readback contents are what the fake list wrote,
not what a GPU would have; the stub `Track` admits every direct list, where the production one can refuse.
