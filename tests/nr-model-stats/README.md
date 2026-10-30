# NR model VRAM stats

Run `python3 tests/nr-model-stats/run.py` from anywhere. It builds in a temporary directory with g++ C++20, ASan and
UBSan and `-Werror`. About two seconds. Design: [model-vram-stats](../../docs/dlssnr/design/model-vram-stats.md).

## What it exercises

1. **Both loaders' stats query, as shipped.** `run.py` slices `dlssnr_query_stats` (with `setULL` and
   `kStatsUnwritten`) out of `retoque/dlssnr/forwarder/dlssnr_forwarder.cpp` and `QueryStats` (with the same two and
   `LastStatsResult`) out of `retoque/dlssnr/DlssNr_DirectRuntime.cpp`, and compiles each into a namespace of
   `cases.cpp` against a fake parameter block (the driver block's vtable: the 64-bit setter at slot 0, its getter at
   slot 8, every other slot null) and fake stats callbacks. Each loader, case by case, and the two must agree:
   - present: the callback read, `SizeInBytes` cleared to `~0`, the callback called with the same block, the size read
     back -- in that order -- and a size above 4 GiB returned whole;
   - absent, a null callback, a block whose getter refuses: 0, nothing written, nothing called;
   - NGX not initialised in that loader, a null block, a null output: 0, the block untouched;
   - refusing: -1, its code kept, no size read; silent (success, nothing written, a stale 777 in the block): -1;
   - faulting: -1, `kFaulted` kept, no size read, and the next query with a good callback does not enter the model.
2. **`retoque/dlssnr/DlssNr_ModelStats.h`**, as it is: MiB rounding (halves up, no wrap at the top of the range,
   "under 1 MiB"), the first build's INFO line and a later build's, every DEBUG line's reason and shape.
3. **Source guards**: the declarations the harness supplies itself (callback type, getter/setter types, slots,
   `kFaulted`) are what both sources say, and `kStatsUnwritten` is `~0ull` in both; the query never populates, loads,
   creates, evaluates, releases or initialises, and calls the callback through `Guarded` into a `volatile` (no tail
   call); `NrState` carries the query and one 64-bit figure; `EnsureForwarder` resolves the export without refusing a
   forwarder that lacks it, and `EnsureDirectRuntime` points at `QueryStats`; `ReportModelVram` forgets the old figure
   before asking, logs one INFO or one DEBUG line and nothing louder, and touches no pass state; it is called once,
   after the build succeeded and after the "running" line; the panel reads the figure in two places (status line,
   export), the export only past its NR-off return; the status line's text has a pt-BR entry.

Mutation-checked on 2026-10-30: dropping the sentinel write, not checking it, asking before initialisation, a
32-bit size, accepting a refusal, never asking, keeping a stale figure and truncating the MiB each fail it.

## What it does not cover

- `Guarded` here is a stand-in that turns a C++ throw into the sticky refusal; the shipped one is SEH, held verbatim
  to the forwarder's by `tests/nr-model-loader`.
- No model, NGX or GPU runs: whether the model publishes the callback into the core's block, what it counts and
  whether its figure is right are for a GPU run ([model-vram-stats](../../docs/dlssnr/design/model-vram-stats.md),
  "What a GPU run must show").
- The menu line is checked as source text, not drawn.
