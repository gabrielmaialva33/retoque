# D3D12 descriptor reverse-reference index (2026-10-30)

Not Linux-specific; filed with the other focused fork records. Code: `retoque/resource_tracking/ResourceReferenceIndex.h`,
used by the upstream-owned `OptiScaler/resource_tracking/ResTrack_dx12.{h,cpp}`.
Test: [restrack-resource-references](../../tests/restrack-resource-references/README.md).

## Problem

With HUD-fix/FG resource tracking active, `ResTrack_dx12` records which descriptor slots reference each resource
whose destruction callback it installed, so that destruction clears those slots. Before this change that was one
`ankerl` map of `std::vector<TrackedResourceSlot>` behind one process-wide `_trackedResourcesMutex`. Every
descriptor view or copy that changed a slot's resource ran `remove_if` over the old resource's vector and
`find_if` over the new one; heap teardown did the same per slot while holding the mutex. A game that copies a
tracked render target's descriptor into ring heaps every frame builds thousands of references to one resource,
so each copy cost O(references) and all copying threads serialised on one lock.

## Source

hoangmaiwaxyz2772-max/OptiGazeScaler (GPL-3.0, like this project) `d1010dbd3e` and `2be1cf1b75` replaced the
same scans with indexed swap-and-pop removal across 64 locked resource shards. The author reports 100,000
randomized and 2,000,000 concurrent tracking operations, and a user reported performance recovered in Death
Stranding Director's Cut. Those commits also carry unrelated late-HUD-less, command-list lifecycle, logging, input
and DXGI work, and a copy path that stops registering copies in the index (lifetime tokens instead); none of that
was taken. Their tracking keeps raw `ResourceInfo*` slots and per-slot position arrays; ours keeps
`weak_ptr<HeapInfo>` plus heap version and index, and a heap object can die with references still pointing at it,
so swap-and-pop with positions stored in each heap would have to write into a heap that may be gone. The idea was
reimplemented rather than ported.

## Design

- `ResourceReferenceIndex<Resource, Heap, 64>`: 64 shards, each a cache-line-aligned `std::mutex` plus a map from
  resource to its references. The shard is chosen by Fibonacci hashing of the address (alignment bits dropped),
  independent of the map's own pointer hash.
- A resource's references are a hash map keyed by (heap version, descriptor index) holding the `weak_ptr<HeapInfo>`.
  Attach (with its duplicate check) and detach are O(1); with `ankerl::unordered_dense` erase is itself a
  swap-and-pop of the dense value vector.
- `ResTrack_dx12` keeps its global name `_trackedResources`; `_trackedResourcesMutex` and `TrackedResourceSlot` are
  gone. `HeapInfo::DetachFromOldResourceLocked`, `AttachToNewResourceLocked`, `DeactivateAndClear`,
  `ResourceDestroyed`, `TrackResourceRelease` and the `HookDevice` reserve call the index.

Preserved: registration lasts from callback installation until destruction, an emptied set stays registered,
attaching to an untracked resource drops the view, one reference per (version, index), `shared_from_this()` is
taken only for a new reference, destruction clears slots outside the index lock with `ClearSlotIfMatches`, and
`_resourceLifetimeMutex` still serialises new registrations. Changed: the order `ResourceDestroyed` visits
references (insertion order before, unspecified now; each slot is cleared independently, so it is not
observable); heap teardown locks each resource's shard per slot instead of one lock for the whole heap (all
descriptor stripes are held throughout, so no slot of that heap can change meanwhile, and a concurrent
destruction's `ClearSlotIfMatches` sees the heap inactive either way). Lock order stays descriptor stripe ->
index shard; no path holds two shard locks or takes another lock under one.

## Evidence

Worktree branch `perf/restrack-sharded-tracking-20261030` on `be4d9cf2`, uncommitted. g++ 16.2.1.

- Characterisation (`cases.cpp`, production slices) passed on `be4d9cf2` before any change, with ASan/UBSan and
  TSan, both map backends; passes unchanged after.
- RED: the differential/stress unit did not compile before the header existed. GREEN after.
- Mutants of the index (detach not erasing, attach registering an untracked resource, take keeping the
  registration) each fail the differential and structure cases.
- `python3 tests/run_all.py --only restrack-resource-references --jobs 1 --strict`: PASS.

Synthetic host timing (`run.py --bench`, -O2, `ankerl` backend, best of 5, shared machine with load ~5; container
work only, not frame time):

| Scenario | Legacy ns/op | Index ns/op |
|---|---:|---:|
| 1 thread, 8 refs per resource | 51.9 | 49.3 |
| 1 thread, 128 refs | 222.4 | 65.3 |
| 1 thread, 1,024 refs | 698.8 | 50.0 |
| 1 thread, 4,096 refs | 4,669.4 | 79.1 |
| 1 thread, 16,384 refs | 25,351.4 | 99.1 |
| 8 threads, 64 refs, distinct resources (wall per op) | 570.4 | 70.3 |

## Not verified

Not built with MSVC, not run in a game, under Wine or on Windows. Real reference counts per resource and the share of
CPU time spent here are unknown for our users' games. Needs an in-game CPU profile (e.g. Superluminal/ETW or
`perf` under Proton) of a HUD-fix/FG title with heavy descriptor copying, Death Stranding Director's Cut if
available, comparing `HeapInfo::SetBy*`/`hkCopyDescriptors` self time and lock wait before and after, plus a
check that HUD-less capture and resource-destruction clearing still behave.
