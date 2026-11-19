# NR composition dispatch

Run `python3 tests/nr-dispatch/run.py`. The runner compiles the production class declaration,
constructor, `DispatchPass`, destructor and the scratch generation against strict host D3D12 fakes
with ASan/UBSan. The constants layout is the real shared header.

Checks 145 dispatches across the 64-slot ring (`DLSSNR_NUM_OF_HEAPS`; `fakes.h` spells it once as
`kRing` and `cases.h` holds the two equal): exact uploaded bytes, isolation between slots,
dispatch dimensions, optional SRV/UAV stand-ins, invalid inputs, and one map/CBV per buffer
instead of per dispatch. Exercises allocation failure, Map failure and a null mapping at
every slot, plus root/pipeline/heap failure and a null device. Each acquired mapping/resource
must be cleaned up once, and a partially initialized shader must refuse dispatch.

## Descriptor reuse

`DispatchPass` creates a view only when the key describing it moved, so this suite asserts descriptor
**contents** and not call counts: a cache that skips a write it owed leaves the previous
incarnation's record in the slot, and counting reports that as a saving. `ViewRecord` in `fakes.h`
carries what each slot was given, `translateTypeless` included, and the fake view creators mirror the
real `Shader_Dx12` signatures so a production call that stops passing an argument is recorded as the
default it actually got.

Four cases cover retirement: six reshapes (one per description field the key carries), an advanced
generation, a saturated one, and a game resource re-created at the same address with the same description
(a new object, so no private-data token: its views must be written again, and a resource that refuses the
token is never reused). Two guards in `run.py` cover what no behavioural test can see while
the view arguments are constants — that every argument a call site passes is also in the key, and
that every member of `BindKey` appears in its own `operator==`.

Design note: `docs/dlssnr/design/descriptor-reuse.md`.

## What it does not cover

CPU preparation and cleanup only: not GPU execution, not whether a reused descriptor is accepted by a
real driver, not frame generation safety, image quality or FPS. Fewer view-creation calls is a count,
not a profile. The existing ring reuse policy is unchanged.

## Frame input contracts

`contracts.py` compiles the production scratch-admission and uniform guide-selection blocks against
`contracts_fakes.h`, with ASan/UBSan. Each mandatory colour allocation fails independently on single
and chain routes at native and reduced sizes. Refusal must park every acquired piece without releasing
it, report the reason, request history reset and allow a later retry. Guide cases fail bytecode
availability, initialization, either allocation and dispatch; a later success must hand the model the
working-size zero-origin pair. Initialized passes and successful guide allocations survive dispatch
failure. All chain histories reset after a refused frame. MatchGuides off and no-needed-resample paths
allocate nothing. Integration guards require the refusal before frame hold, model and resolve.

The compiled slices are the production decisions; the surrounding model/evaluate and resources are
fakes. This does not execute the complete renderer or establish actual pixel preservation on a GPU.
