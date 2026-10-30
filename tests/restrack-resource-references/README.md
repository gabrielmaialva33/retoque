# D3D12 resource reverse-reference tracking

```sh
python3 tests/restrack-resource-references/run.py           # suite (ASan/UBSan and TSan)
python3 tests/restrack-resource-references/run.py --bench   # synthetic timing, not pass/fail
```

`ResTrack_dx12` keeps, for each resource whose destruction callback it installed, the descriptor slots
that reference it, so a destroyed resource's views can be cleared. The index is
`retoque/resource_tracking/ResourceReferenceIndex.h`; see
[the change record](../../docs/linux-proton/restrack-reference-index-20261030.md).

Three parts, each built with g++ under ASan/UBSan and again under TSan:

- **Production slices** (`cases.cpp`, `fakes.h`). `run.py` slices `ResourceInfo` from `Hudfix_Dx12.h`, the
  tracking globals and `HeapInfo` from `ResTrack_dx12.h`, and `ResourceDestroyed`/`TrackResourceRelease` from
  `ResTrack_dx12.cpp`, verbatim, against fake resources whose destruction notifier records and later runs
  callbacks. Cases observe only what slots return: views of untracked resources dropped, single callback
  registration (and failure paths), metadata refresh, overwrite/clear then destroy, 4096-slot heaps, heap
  teardown, expired and replaced heaps, shutdown. `slice-stress` runs 8 threads of views/copies/clears with
  per-thread resources destroyed and re-registered meanwhile; destroying every shared resource afterwards must
  leave no slot bound (a lost reverse reference fails). These cases passed unchanged on the code before the
  index (`be4d9cf2`), which is what makes them a characterisation.
- **Differential** (`index.cpp`, `legacy.h`). `legacy.h` is the former single-mutex vector bookkeeping, statements
  kept. Four seeds x 100,000 random operations (register, contains, attach, detach, take, heap replacement,
  bursts on one hot resource, stale heap versions) on both; return values, `Take` results and per-resource
  state are compared as multisets after every operation (all resources every 997). Order is excluded on
  purpose: the index does not keep insertion order, and production clears slots independently.
- **Stress** (`index.cpp stress`). 8 threads x 250,000 rebinds (2M; TSan 8 x 50,000) on shared resources with
  disjoint slots, plus a thread registering/taking other resources the workers attach to; the final reference
  sets must equal each thread's own record exactly.

The map backend is `ankerl::unordered_dense` (production) when `external/unordered_dense` is checked out, and
always a `std::unordered_map` stand-in (`shim/`), because host CI checks out only simpleini, spdlog and vulkan.
Without the submodule the run prints a NOTE and covers only the stand-in.

Not covered: D3D12 runtimes, real `ID3DDestructionNotifier` callback threading, the hooks that call `HeapInfo`
(`hkCopyDescriptors`, view creation, heap release), MSVC codegen, and any frame-time effect. `--bench` times
container work only.
