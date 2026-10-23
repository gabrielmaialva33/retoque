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
  over a freed heap, is a violation. Executing a list writes each source's bytes into the readback memory at
  once, as a GPU would have, and notifies the model; nothing tells the scan the data is there unless the
  case completes the fence.

## Cases

| Case | What it holds |
|---|---|
| `late` | 64 zero buffers fill the list, then a moving exposure: it is held, the zeros give way as false, it is found |
| `recreate` | a new set created while the list is full, the feature released, the old set let go: the new exposure is found, every old buffer is released, none is copied after it was let go |
| `long-lived` | an exposure kept across a feature release stays held and found, its readings carried on |
| `attribution` | a newcomer is never credited with a value its own buffer did not hold: after a release, and after a late completion of a copy of a let-go candidate at the same position |
| `integer` | an integer buffer (65536 reads as `9.18e-41`) gives way first; 63 plausible quiet buffers are kept |
| `fence-never`, `fence-signal`, `fence-removed` | a fence that never completes, a failed Signal, a removed device: no reading, no slot rewritten, a let-go candidate with unconfirmed copies retained |
| `discarded` | a list reset without executing frees its slot unread; readings resume when lists run |
| `reuse` | let go, retained while its copy is unconfirmed, released after it, and a new resource at the same address adopted fresh and found on its own values |
| `placed` | the game releases a placed exposure and its heap: nothing copied after, nothing destroyed out of order, both released by the end |
| `view` | a candidate seen only through a view is released at the boundary, as before; a committed one is kept |
| `inert-off`, `inert-unwanted` | NR off, or the scan not the white point's source: no reference, no copy (and with NR off nothing examined) |
| `untracked` | a list the tracker refuses gets no copies, and the status says why |

Source guards then check the call sites: the placed creation hook passes its heap, `ReleaseFeature` calls the
boundary once and before the feature is torn down, each `Tick` probes before it sweeps, the sweep tracks the
slot and every candidate before recording, readings come only from completed recordings and never from elapsed
sweeps, and the boundary releases view candidates without forgetting the rest.

Against the code before this change (`acdba2af`) every case fails except the two inert ones, and so do the
guards that name the new structure.

## What it does not prove

Nothing here runs D3D12, a driver or a GPU. Real reference counts under vkd3d-proton or Windows, whether a
runtime keeps a placed resource's heap alive, Streamline's or NGX's own lifetime rules, and whether the scan
finds any particular game's exposure are all outside it. The readback contents are what the fake list wrote,
not what a GPU would have; the stub `Track` admits every direct list, where the production one can refuse.
