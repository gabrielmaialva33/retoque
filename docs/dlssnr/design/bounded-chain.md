# Bounded D3D12 model chain

Implementation review, 2026-09-05. Sources: PR #23 separate histories/admission and PR #26
sparse settings. This adapts their behavior to both current D3D12 NR stages and preserves
the original codec/proxy rather than importing either stale shader pipeline.

## Contract

- One to ten passes since 2026-11-11 (one to three before; see "Ten passes" below), default one/master.
  Each extra pass owns its NGX feature/history.
  `modelInput` remains the untouched working-resolution proxy. Outputs alternate between
  `output` and a private working-resolution `passPing`; never write into `modelInput`.
- Only successful evaluations advance the selected answer. Later failure stops the chain,
  retires the failed instance, latches retry and composes the previous successful answer.
  All intermediate read states return to UAV. The final selected answer is transitioned
  for composition and restored; supersampling downsamples that answer exactly once.
- A locked snapshot resolves master/individual settings consistently. Changes settle for
  500 ms; working dimensions/format also settle for 500 ms. Only changed instances rebuild;
  upstream rebuilds reset downstream histories. Creation spacing is at least 500 ms and
  one creation per observed present. Evaluation is at most once per observed present.
- Every newly created feature additionally waits until a later present AND its recorded
  creation has actually been submitted and completed. Present advancement alone is not
  GPU completion. A missing/failed/unknown submission never satisfies this gate.
- Native Vulkan and driver proxy remain one/master. Opting in after any legacy untracked
  NR work requires restart; it cannot retroactively create lifetime evidence. Startup with
  one/master installs no new submission tracking and preserves original model arguments.
  Once activated, tracking stays active even if count returns to one.

## Resource evidence and limits

> **Superseded 2026-10-19** by [chain-in-flight.md](chain-in-flight.md) for admission and tracked
> retirement: a new recording now needs earlier ones only finished on the GPU (or at most three in
> flight on one queue), not Reset by the game, and parked objects drain on fence plus age. The text
> below describes the original rule and why it stalled in game.

Before starting a NEW experimental recording, all preceding NR usage must be Ready:
successful Reset sealed the epochs against replay and every observed GPU fence completed.
This CPU admission gate prevents overlapping writes/history use across queues and upload
ring rewrites while an older NR recording could still execute. A lease explicitly owned by
`ScopedPreUpscale` may be borrowed by its inner Dispatch and remains alive through the
upscaler evaluation and scratch-restoration barrier; another scope cannot borrow just by
using the same command-list pointer. Scope exit releases only its own token, including early
returns and exceptions. Default one/master startup does not allocate this lease.

Before recording admitted NR work (including the pre-upscale scratch loan), the renderer
tracks that command-list epoch. Retired textures, features and replaced supersampling scaler
objects copy a conservative snapshot of all NR usage. They are freed only when each epoch
is sealed by successful Reset and every observed submission fence is complete. Completed
but replayable lists are not safe to release. Shutdown retains all objects if any current
or retired tracked usage is unresolved; it does not fall back to evaluation-count retirement.

A retirement admission cap refuses work before a recording/allocation when 32 parked
objects remain. One admitted recording can add its bounded cleanup batch before the next
admission check. This prevents repeated slider/DRS changes on an unsubmitted epoch from
creating unlimited retired generations. Unknown or missing completion retains memory and
can stop NR until recovery/restart; retention is intentional, not successful reclamation.

Adapter-local budget and current usage are queried before opted-in generation replacement
and each additional model. Admission reserves max(512 MiB, 20% budget), 150% of the measured
base-model usage delta (minimum estimate 256 MiB), and conservative texture storage.
These are measured-budget heuristics, not a guarantee: NGX allocations are opaque, allocations
may become visible asynchronously, and other processes can consume VRAM after admission.
Allocation/create/evaluation failures latch instead of repeatedly attempting the same work.

Waiting for Reset/fences skips NR rather than blocking the game. The model may therefore
run less frequently and throughput may decrease. Stage coverage logs must distinguish
those skips from model execution; a faster average caused by skipped frames is not a
model optimization. This is conservative serialization, not a high-throughput resource-ring
implementation. The admission decision and resulting refusal are INFO-visible.

The inherited one/master startup path still uses 32 evaluations for retirement; this new
opt-in does not silently change that default. Existing descriptor/constant-buffer rings,
meter/capture readbacks and GPU timer readbacks have their existing reuse/read policies.
The new experimental Ready gate prevents reuse across incomplete/replayable NR recordings;
it does not claim a runtime GPU validation of every readback policy or that a changed device
can safely reuse the module's existing objects. No in-game DRS stress
or general device-recreation safety is claimed by this change. Restart on device replacement.

## Adversarial review and evidence

Checked: one-pass/master evaluate call remains literal and passes the baseline verifier;
shader constants/HLSL/both compiled headers untouched; config comes from the four-point
snapshot owned by Config; original proxy never becomes an output; pass 2 failure keeps
pass 1 output, pass 3 failure keeps ping; odd/even final answer used by the only down-leg;
separate histories, settings debounce and downstream reset; fail latch prevents creation
storms; unknown submission cannot age into release; shutdown cannot release unresolved
tracked objects; retirement cap precedes new work; invalid pre-upscale exposure is cleared
before its outer transition scope, matching the inner texture contract.

`tests/nr-multipass` passes production portable routing/scheduling/admission helpers and
submission model under ASan/UBSan. It does not execute actual NGX/D3D12 or prove rendered
quality, GPU performance, real driver submission coverage or synchronization correctness.
Per-pass INFO cost is explicitly CPU-call time. Existing NGX GPU timing spans the full chain.

A diagnostic-only INFO spatial-contract report records Color allocation dimensions, the
render-subrect values and individual NGX query success/absence, game output, requested
model dimensions and WorkingScale. Identical contracts do not log again. A before-stage
model at least as large as game output says `no spatial reduction`, without inferring DLAA
from allocation size. The boundary suite executes that reporter and checks query provenance,
no repeat for an unchanged contract and no parameter/resource writes from diagnostics.

## Ten passes (2026-11-11)

**Decision.** The owner asked on 2026-11-11 for up to ten sequential D3D12 passes, as the NeuRotic fork offers
(`OptiScaler/shaders/dlssnr/NrCompositionPool.h`, `MaxPasses = 10`, commit `beae4eea`). The roadmap had parked
ten-pass chains as "not adopted simply because another project advertises them"; this is an explicit request, not
adoption by advertisement, and the cost below is stated where the count is chosen.

**What changed.**

- `[DlssNr] Passes` is bounded to 1..10 (`DlssNr::PassConfig::MaxPasses`, `DlssNr_PassSettings.h`); above ten is
  ten, malformed or overflowing text is the default. The default stays one. `BoundCount` is unchanged on 0..3, so
  every count that was valid before resolves to the same number, the same snapshot and the same loops.
- `[DlssNr] RRPasses` keeps its own bound of three (`MaxRRPasses`, `BoundRRCount`): ten was asked for `Passes`
  only, and that route reaches `RRWorkingScale=2.0`, where one instance measured about 1.8 GB
  ([chain-in-flight](chain-in-flight.md), "Rebuild headroom"). Native Vulkan and the driver proxy stay one/master.
- The per-pass bookkeeping in `NrState` (`passFeature`, `passBuilt`, `passReset`, `passFailed`, `creation`,
  `creationFrame`) is sized by `MaxPasses` and every loop over it stops at that bound or the requested count. These
  are pointers, flags and settings, not GPU objects.
- GPU timing records one model pair per pass (`GpuTiming::Detail::MaxModels = MaxPasses`): a slot is 22 timestamps
  and a 176-byte readback instead of 8 and 64, allocated only while `GpuTiming` is on.
- Menu: "Requested passes" goes to ten; the existing "Edit pass" selector reaches any pass, so ten passes are ten
  positions of one control, not ten stacked blocks. From two passes up a line under the count reads
  "Cost: Nx the model's GPU time and N model instances in video memory."; the tooltip says the same and that extra
  passes are built one at a time and refused when the adapter's budget cannot hold them.
- The offline-replay validators accept `requested_passes` up to ten ([offline-replay](offline-replay.md)).

**What did not change, and why that is enough.**

- Allocation follows the configured count, not the bound. A feature for pass `i` is created only when `i < Count`,
  one per stable observed present and at least 500 ms apart, each after the adapter-budget admission above
  (`Chain::Admit`). Lowering the count parks the features above it. A refused pass latches and the chain runs the
  passes it has; it retries when that pass's settings change.
- Two working surfaces carry any chain length: answers alternate between `output` and `passPing`
  (`Chain::Routing`), so no texture is added per pass.
- No descriptor or constant slot is added per pass. Extra passes record only barriers, `SetExtras` and the
  model's evaluate ([chain-in-flight](chain-in-flight.md), "What the gate actually protected"); the composition
  runs once, after the last pass, so the 64-slot ring's six dispatches a frame are the same at one pass or ten.
  NeuRotic reserves `SlotsPerAdditionalPass = 4` composition slots per extra pass because its extra passes compose
  on their own; ours do not, so there is nothing to grow.
- No shader, constant buffer or HLSL layout changed. No lifetime rule changed: nothing new is created, parked or
  freed, and features still retire through the same tracked parking.
- The retirement cap stays at 32. A rebuild retired at most 18 objects with three passes; each pass past the third
  adds one feature, so a worst-case rebuild at ten passes (with the other format's set and Transfer 2's bundle) can
  park 32 entries, and the pass then skips with "Retired resource limit reached" until they drain (finished plus
  32 ticks). Raising the cap would instead let more retired model instances, each hundreds of megabytes, wait at
  once.

**Cost (expected from the design and earlier measurements, not measured at ten).**

- GPU time: every pass is a full model evaluation at the working size, so model time grows about linearly: ten
  passes are about ten times one, plus one composition. The model's share of a frame is in
  [model-cost-across-architectures](model-cost-across-architectures.md).
- Video memory: one model instance per pass. One instance measured 578 to 767 MiB at a 2292x960 working size on an
  RTX 4090 ([model-vram-stats](model-vram-stats.md)), so ten passes there are roughly 6 to 7.5 GB of instances on top
  of the game. On a 12 GB card the admission is expected to refuse the later passes; on 24 GB it may hold all ten.
- Ramp: the extra passes are built at least 500 ms apart, so ten passes reach full depth at least 4.5 s after a
  change, and a resolution change starts that ramp again (a pass's own preset change rebuilds that pass alone and
  resets the histories after it). Until then fewer passes
  complete than are requested, which the status line and the INFO log say, and GPU timing rejects those samples as
  partial chains.

**Still to measure on a GPU (not run for this change).** Release build and host suites only. A game run on an RTX
3060 (12 GB) and an RTX 4090 (24 GB) must record, at Passes 1, 3 and 10 with `GpuTiming=true`: the confirmed model
and total milliseconds; how many passes complete and at which pass admission refuses (INFO `create pass N result`
and the "Additional pass refused" status); the adapter's usage from the admission lines; a resolution change at ten
passes (the skip until the parked features drain must end); lowering ten to one (nine features parked and freed);
and whether ten passes of the same model help the picture at all or only compound its edit. Native Windows
validation is required as for any new D3D12 pass count.
