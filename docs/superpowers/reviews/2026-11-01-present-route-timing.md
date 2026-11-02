# D3D12 Present route timing — implementation and validation

Date: 2026-11-01. Base: `73fad086eef72bb2e07845d751c9d6d2d514425c`.
Branch: `feat/nr-present-timing-20261101`, isolated worktree, uncommitted.
User authorized implementation after the engine-inputs/upscale/Present study. No game installation,
INI/model change, commit, merge or push was performed.

## Delivered behavior

- Existing opt-in GPU measurement now records the complete GPU command interval of D3D12
  RunPresentPass, including zero-guide clear when used, backbuffer copies, synthetic motion,
  Dispatch, HDR decode/encode and resource restoration.
- Guide capture has an independent two-timestamp interval on its producer list.
- Independent eight-slot pools/counters/phases preserve the existing Evaluation/Conversion pool.
- Each accepted sample uses the existing sealed-single-execution-fence certificate and actual
  queue frequency. Unknown completion retains resources; full pools drop measurement only.
- Sample metadata records host input source (game, synthesized motion, zero), ring sequence,
  capture age at selection, depth allocation/output extents, observed Present ordinal, boundary,
  HDR and whether EvaluateAtPresent recorded work.
- Panel and copied diagnostics show separate historical samples, including without an Evaluation
  sample. Optional pt-BR translations cover the new text and preserve printf signatures.

The total-only two-marker design is deliberate. It does not subtract an independently sampled
Dispatch value. Present already contains Dispatch/HDR; guide capture can belong to another
recording/queue. None of these values are added automatically. CPU waits, queue delay before the
first timestamp and display latency are excluded.

## Verification

| Check | Observed result |
|---|---|
| Baseline before changes | nr-gpu-timing, nr-timing-boundary, nr-diagnostic-report PASS |
| RED runtime cases | Missing Region API caused compilation failure before production implementation |
| Production timing host | 31 portable cases + 3000 epoch cycles, 24 Evaluation runtime cases, 6 cadence, 6 HDR conversion, 13 new Region cases PASS under ASan/UBSan |
| RED UI/export cases | New UI assertion failed before readout implementation; export compiled only after new sample DTO existed |
| Timing metadata/UI | 36 cases PASS; production metadata/readout extracted and executed |
| Diagnostic export | Behavioral cases and read-only/privacy source guards PASS |
| Guide ring | In-flight selection/age cases and 20,000-frame simulation PASS |
| Guide identity | 1,235,154 rule checks and 253 production-glue checks PASS |
| Present sync | Existing auto/explicit wait policy checks PASS |
| Submission | 25 lifetime cases, 3000 retirements, 1000 concurrent reset/notify pairs and hook-order guards PASS |
| Renderer module ownership | Shared objects/functions and 27 present-guide owner/body contracts PASS |
| Localization | Real parser/ImHashStr, printf/UTF-8, 578 dictionary entries and four runtime-loader modes PASS |
| Release DLL + forwarder | MSVC under Wine, exit 0; both outputs produced |
| Real D3D12 | First run PASS: nested Present accepted twice, two replay cases rejected; separate producer capture accepted; blocked queue withheld until released; disabled negative control rejected |
| Final adapter-specific receipt | PASS on NVIDIA GeForce RTX 4090, vendor 10de/device 2684, identified from the actual D3D12 adapter LUID via DXGI |
| Final strict host selection | 9/9 suites PASS, process-exit evidence; receipt in x64/logs/present-timing-host.json |
| Formatting/documentation | clang-format 22.1.8 dry-run, git diff --check and local documentation/catalog check PASS |
| Independent code review | No material defects found in current source packets; separate lifetime/submission and integration/UI passes; static review, tests not independently rerun |

The initial Release invocation failed on a missing legacy SPIR-V header in the new worktree.
The existing local prerequisite was copied into that worktree; the next invocation succeeded.
No source workaround was needed. Build retains the existing LNK4098 library-conflict warning.
The host logging stub was corrected to evaluate/type-check log arguments rather than erase them.
Only CaptureGuides' body digest was updated after its additive instrumentation was reviewed;
all other body pins and mutation controls remain. The guide-identity test's adjacency anchor was
updated to preserve the same track/count/unlock ordering around the newly inserted scope.

A repeat harness preparation exposed read-only (0555) Proton DLL copies in its owned staging
directory. The harness now stages copies under a unique temporary name and replaces its own
destination atomically, without changing installed runtime permissions. The repeated harness then
passed with the actual adapter identified. The failed preparation remains in
`x64/logs/present-timing-d3d12-final.log`; it failed before GPU execution.


## Build and evidence identity

Release output hashes:

- `x64/out/OptiScaler.dll`: `0e371eca72293623edf4b4b96f2789f6a47adbe3e3cce7cd98117ceb6ac9d1a5`
- `x64/out/nvngx.dll_dlssnr.dll`: `94d58bec380fb98431ed8c8291f2e3e82b4247c3ab0ca585e926147a642cf7f1`

Local ignored evidence:

- `x64/logs/present-timing-build.log` and `present-timing-build-2.log`.
- Build supervisor receipts under `x64/logs/20261101_173650-381867-cb1f8df1/`.
- `tests/nr-gpu-timing-d3d12/artifacts/result.json`: status, source/binary SHA256 manifest and scope limits.
- Harness `build.log`, `run.log`, `negative.log` and isolated runtime/compiler prefixes under the same artifacts directory.
- Final harness console record: `x64/logs/present-timing-d3d12-final-2.log`.
- Final D3D12 result SHA256: `9991a2d2320f0318474c32c7bc395f5d721991c879cb8b9b9d6488f7f441ecef`.
- Final strict host receipt/log: `x64/logs/present-timing-host.json` / `present-timing-host.log`;
  individual suite logs retained under `x64/logs/present-timing-host-suites/`.

## Review and limits

Herdr Codex independently audited the baseline design/call sites. Its recommendations on separate
pools, exact selected-guide provenance, device ownership, sealing before Close/Drop and no automatic
summation are implemented. Its optional four-marker decomposition and CPU wait telemetry are outside
this total-only slice. The fresh reviewer found no material defects in the current source packets,
including the separate lifetime/submission pass, renderer scope placement, provenance and UI/export.
The first remote read had stalled; the reviewer then inspected complete local copies of the relevant
current code and explicitly superseded the incomplete review. Test/build/GPU evidence remains the
implementer's verification. Review covered the instrumented renderer excerpts, not unrelated code.

These tests exercise actual D3D12 timing/Submission with a ResTrack shim and GPU buffer copies.
They do not run NGX, production RunPresentPass, an in-game menu, native Windows or a performance A/B.
The native D3D11/Vulkan Present hosts do not have the new route bracket. Capture labels describe the
shared D3D12 copy core, not earlier bridge transport. Input source is the host's selected resources;
later GuideMatch/periphery/cadence transforms inside Dispatch are not claimed here. Ring sequence
can restart after ring reset and is not a global capture ID. Use fixed settings and external model/
DLL identities for comparisons.

Primary API basis: [D3D12 timing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing)
and [ResolveQueryData](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resolvequerydata).
No FPS gain or new rendering mode is claimed by this instrumentation change.

## Follow-up review fixes (2026-11-02)

An independent Codex review of the frozen packet (patch sha256 `913bd7f7…`) found no P0/P1 and two P2s.
Both are fixed on top of that packet:

1. **Disabled path took a new mutex.** RunPresentPass and CaptureGuides called
   `GetDlssNrGpuTimingSettings()` (which locks `_dlssNrPassSettingsMutex`) and filled metadata even with
   GpuTiming off. Both now check `Config::DlssNrGpuTimingRequested()` first: a lock-free mirror of the setting,
   stored under the settings lock wherever the setting is written (INI read, `SetDlssNrGpuTimingEnabled`).
   With timing off they read no settings, fill no metadata and construct no region; the getter stays
   authoritative when it is on. A first fix, a "timing was ever enabled" latch in GpuTiming, was rejected by
   the second Codex review (P2): with `GpuTiming=true` from the INI and no Evaluation (capture-only, e.g.
   RequireDlss refusing the Present), nothing armed it and capture samples never started.
   `nr-gpu-timing-config` now checks the mirror cold after the INI read and after each toggle.
2. **No regression for Finish after Close.** `nr-timing-boundary` now guards, in source, Finish before
   `SubmitPresentList`, `presentTiming.reset()` immediately before every later `DropPresentList`, capture
   Finish after `CommitRecord`, and both mirror gates. Six mutants (including Finish-after-Submit and
   reset-after-Drop, which the previous runner accepted) are rejected. This is an order guard; a fixture
   executing RunPresentPass with fakes remains future work.

`DlssNr_GpuTiming.{h,cpp}` are unchanged from the reviewed packet. Only the CaptureGuides body digest in
`nr-dx12-internal` was updated again for the gate. The decomposition
into four timestamps and CPU-wait telemetry remain a separate, not yet approved, design.
