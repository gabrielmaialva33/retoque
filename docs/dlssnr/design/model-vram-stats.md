# The model's own VRAM count

Status: built 2026-10-30 on `feat/nr-vram-stats-20261030`; host-tested by `tests/nr-model-stats`,
`tests/nr-model-loader` and `tests/nr-diagnostic-report`. As first built it never produced a figure (see "The slot the
size is written through"). After the 2026-10-31 fix, one synthetic run only: the `tests/dlssnr-loopback` default sweep
(Proton, RTX 4090, forwarder loader, model `6eb209e7…`) logged 767 MiB (803908096 bytes) at the first build and
1190 MiB (1247838208 bytes) at the two later builds. No game, no `ModelLoader=direct`, no other model version or
preset; nothing here says what 310.9.16.0 reports.

## Why

NVIDIA says the October weight set (310.9.16, [model-310-9-16](model-310-9-16.md)) needs about 20% less VRAM. Nothing
in this fork could check that. The one memory number it had, `g_nr.measuredModelBytes`, is the DXGI budget's
`CurrentUsage` before and after the build, taken only with the bounded chain on ([bounded-chain](bounded-chain.md)):
it counts whatever else the process allocated in between and is not logged on its own.

The model counts its own allocations. Its strings, read-only from the files (not copied), list a stats callback beside
the scaling-ratio callback the fork already asks ([ReportScalingRatios](../../../retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp)):

```text
DLSSNRGetStatsCallback
DLSSNRComputeScalingRatioCallback
...
GetStatsCommon
Error: couldn't find initialized instance
SizeInBytes
ComputeScalingRatioCommon
```

All three of `DLSSNRGetStatsCallback`, `GetStatsCommon` and `SizeInBytes` are present in 310.8.2.0 and 310.9.16.0
(checked 2026-10-30 on five local copies, two of 310.8.2.0 and three of 310.9.16.0, by version resource and byte search).

NVIDIA's RTX Remix calls it (`NVIDIAGameWorks/dxvk-remix` commit `09808cc70`, 2026-10-16,
`src/dxvk/rtx_render/rtx_ngx_wrapper.cpp`, `queryDlssNeuralRenderingVramAllocation`, MIT), right after
`NGX_VULKAN_CREATE_DLSSNR_EXT1` succeeds, on the same parameter block the create used:

1. `NVSDK_NGX_Parameter_GetVoidPointer(params, NVSDK_NGX_Parameter_DLSSNRGetStatsCallback, &callback)`; a null
   callback is treated as not supported;
2. `callback(params)`, an `NVSDK_NGX_Result`;
3. `NVSDK_NGX_Parameter_GetULL(params, NVSDK_NGX_Parameter_SizeInBytes, &bytes)`.

Remix shows it as "DLSS-NR SDK VRAM Allocation: N MiB" (bytes / 1024²), with the tooltip "SDK-reported aggregate
allocation, queried at feature creation. Excludes Remix-managed input/output textures."

## What it does

| Where | What |
|---|---|
| `dlssnr_query_stats` (forwarder), `DirectRuntime::QueryStats` | Only once NGX is initialised in that loader. Read `DLSSNRGetStatsCallback` through the block's pointer getter (slot 8, as the ratio callback is read); write `~0` into `SizeInBytes` through the unsigned 64-bit setter (slot 7); call the callback through `Guarded`; read `SizeInBytes` back through the unsigned 64-bit getter (slot 15). 1 with the size; 0 when no callback is published; -1 when it refused, faulted or left the size unwritten. No `PopulateParameters`: the ratio query published the callbacks before any feature existed, and publishing again over a live feature is not something it showed harmless. |
| `ReportModelVram` (`DlssNr_Dx12_Model.cpp`) | Called once after each successful D3D12 build, after the "DLSS-NR running" line, under `g_nrMutex`. One INFO line with a figure: `DLSS-NR: the model's feature uses N MiB (B bytes, its own count)`; at a later build "features ... at this build; a feature still being retired may be in it". Without a figure, one DEBUG line for the process saying why (old forwarder, no callback, refused with its code, no size, fault). |
| `DlssNr::ModelVramBytes()` | The last figure while that feature runs; empty on native Vulkan, with the pass off or failed, or when the model gave none. |
| Panel status line | "(model: N MiB)" after "Running." on D3D12, only when there is a figure: a statement, never a control (DEVELOPMENT.md rule 2). pt-BR entry added. |
| Copy NR diagnostics | Detected: `Model VRAM: N.N MiB (B bytes), the model's own count at its last build`, or `Model VRAM: not reported (...)` ([diagnostic-export](diagnostic-export.md)). |

The pure text is `retoque/dlssnr/DlssNr_ModelStats.h`.

## The slot the size is written through (2026-10-31)

The first build asked through the wrong pair. A loopback run at `LogLevel=1` (`tests/dlssnr-loopback`, default sweep,
model `6eb209e7…`, five creates) logged `no VRAM figure from the model (its stats callback answered without a size)`:
the callback answered success and the size read back was not there. The model's `GetStatsCommon` (disassembled
read-only) asks the feature for its count, then writes `SizeInBytes` through the parameter block's vtable offset
`0x38`, slot 7. MSVC lays a class's overloaded virtuals out in reverse declaration order, which is also why the float
setter is discovered at slot 6 rather than the header's 1: slot 7 is `Set(unsigned long long)` and its getter, the one
Remix's `NVSDK_NGX_Parameter_GetULL` reaches, is slot 15. Slots 0/8, which the fork calls its 64-bit pair, are the
`void*` pair; the driver's block keeps the type a value was written with, so the figure was never visible through
slot 8. Both loaders now clear and read the size through 7/15; `tests/nr-model-stats` types its fake block the same way
and writes the size through slot 7, as the model does.

## Invariants

- NR off: nothing changes; the query is reached only from a successful build.
- No figure, a refusal or an old forwarder never changes the pass: no `failed`, no reason, no rebuild. The export is
  resolved optionally, like the ratio query, and `dlssnr_abi_version` is unchanged (no evaluate argument list changed).
- **A fault inside the callback is the one exception, on purpose.** It is a call into the model like any other, so it
  goes through the same `Guarded` and the same sticky fault state: after it, neither loader enters the model again,
  because the model may be holding its own lock ([`dlssnr_forwarder.cpp`](../../../retoque/dlssnr/forwarder/dlssnr_forwarder.cpp),
  "Fault containment"). `NoteModelFault` reports it at that call instead of at the next evaluate.
- The forwarder and the direct runtime stay identical: `tests/nr-model-loader` holds their parameter writes (64 now,
  with the `setULL` sentinel) and model calls equal, and `tests/nr-model-stats` runs both queries against the same fakes.

## What it does not tell

- The count is the model's, at the moment of asking. Remix calls it an aggregate: with the bounded chain, extra pass
  features are built later and are not in the first figure; after a rebuild, a feature still retiring may be in it.
- It excludes the fork's own surfaces (proxies, copies, guide resamples), as Remix's excludes its textures.
- Native Vulkan (after-upscale route and present host) does not ask yet: it would need the same query on the Vulkan
  parameter block, in two build sites, one of them on a builder thread.
- The call comes right after `CreateFeature` recorded into a command list that has not executed yet. Remix does the
  same; whether the model counts allocations at create or at first execution is not known.

## What a GPU run must show

1. Build with MSVC; in a D3D12 title, the INFO line after "DLSS-NR running", on both `ModelLoader=forwarder` and
   `ModelLoader=direct`, with the same figure for the same model, preset and size.
2. 310.8.2.0 against 310.9.16.0 at the same size and preset, and 310.9.16.0 Preset 1 against Preset 2: whether the
   October set is near NVIDIA's "-20% VRAM" by the model's own count, and how the count moves with working scale.
3. The figure against `measuredModelBytes` with the chain on, and against Remix's tooltip on the same GPU if available.
4. A forwarder from before this change beside the new OptiScaler.dll: NR runs, and only the DEBUG line appears.
