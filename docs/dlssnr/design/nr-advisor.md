# In-game NR cost advisor

Design first, 2026-11-19. Inspired by the idea of a reversible in-game trial in NeuRotic;
no code, tables or translations are taken from that project. This is a cost experiment,
not an image-quality assessment. Implementation and host contracts are not game qualification.

## Safe trial boundary (source at 6972e2d7)

| Setting | D3D12 after upscale / Present / D3D11 bridge | Native Vulkan |
|---|---|---|
| WorkingScale | Trial a local Dispatch override. `DlssNr_Dx12.cpp:1312` resolves it each call; `:1365-1400` settles extent changes before reconstruction. Existing retirement owns resources; advisor never frees them. RR and before-upscale excluded. | Runtime resolves scale (`DlssNrFeature_Vk.cpp:1790`), but no fence-confirmed GpuTiming stream: no trial. |
| Cadence | Trial a local requested-cadence override at `DlssNr_Dx12.cpp:1905`. Existing refusal/signature/reset logic `:1927-2019` remains authoritative. Adaptive cadence excluded (effective cadence would differ); no FG trial (render versus generated frame ambiguity). | No matching instrumentation/cadence path: no trial. |
| Passes | Not trialled. `DlssNr_Dx12_Lifetime.cpp:300-357` may promote the process from legacy to tracked ownership, irreversibly for the session. Restoring a number cannot restore that state. Existing count remains constant. | One master pass only; no trial. |
| Stage 0/1 | Not trialled. Routing happens outside Dispatch, while the safe temporary override seam is inside its renderer lock. A Config write would affect other evaluation threads and save paths. Only Stage 0 is admitted. | Needs a restart, not trialled: `DlssNrFeature_Vk.cpp:2878` refuses startup placement changes after Post claimed the process. |

Paths above are under `retoque/shaders/dlssnr/` except `DlssNrFeature_Vk.cpp` under
`retoque/dlssnr/`. D3D11 bridge means the shared D3D12 Dispatch, not native D3D11 model execution.
The experimental native-D3D11 scene transport, driver proxy, RR, peripheral compression,
frame generation, hold-frame, capture, and non-default stage are excluded. Present trials
measure Dispatch, not caller copies, HDR conversion, waits or the whole Present region.

## Pure policy and measurements

`DlssNr_Advisor.h` owns no globals or graphics types. States: idle, settling, sampling,
restoring, done, aborted. Candidates are deterministic: current effective scale/cadence,
then lower scales from 0.75/0.5/0.25, then cadence 2 and 4 at the original scale (only if
larger than the original cadence). No combinations or settings that raise the cost are tried.
Pass count and placement remain the player's. Quality preference is original order, an
explicit conservative heuristic, not measured picture quality.

Requires existing GPU timing enabled, interval 1, and a recent confirmed Stage-0 sample.
Timing off/unavailable: explanation only, no Start control; trial never enables or saves it.
Each candidate has a distinct token stamped on recorded timing metadata. Only the timing
collector after its existing fence/recording certificate supplies samples; it supplies EVERY
accepted sample rather than the latest-snapshot UI cache. Old tokens cannot contaminate a
new candidate. A recording-ID watermark also rejects late settling recordings after sampling starts.
Duplicate sample IDs, reset/capture frames, NaN/Inf/nonpositive time and missing
real-frame intervals are rejected. Tokens remain unique across runs. Metadata records the
interval between consecutive real NR evaluations; duplicated or nonconsecutive present IDs are not intervals.
This is observed base-frame pacing at the NR seam, not display refresh or generated FPS.

Settle for at least 1 second AND 30 confirmed samples, then collect 60 confirmed samples.
Per candidate timeout is 20 seconds; retain the runtime's last refusal verbatim, or report
missing confirmed timing. A refusal during normal reconstruction is not immediately terminal.
Record median Dispatch ms, mean of the slowest ceil(N*0.05) Dispatch samples, median observed real frame ms, and 100 * median Dispatch / median real frame ms.
This is a window-level share, not a same-frame pairing: entry-to-entry pacing observes the
preceding frame, and CPU/GPU overlap prevents identifying a GPU critical-path share. Do not add overlapping
GPU regions or infer no-NR frame time by subtracting GPU duration from a CPU wall interval.

Recommend the first candidate in preference order whose median observed frame interval is
at most 1000/target-FPS. No qualifying candidate means no recommendation. It is a measurement
of that scene, not a guarantee or a counterfactual FPS prediction. Pool saturation, ambiguous
submissions and skipped evaluations can limit coverage; the report shows accepted counts.
Scene/motion/CPU/limiter changes require a new trial. Model cost alone cannot diagnose them.

## Restoration and lifetime

Trial settings NEVER enter Config (including CustomOptional's volatile backing state), so
Save Settings and a process crash cannot persist them. Dispatch copies an optional override
under the advisor mutex and uses it only for that call. The player values, including auto/unset,
remain bit-for-bit untouched. Restoration clears the override; normal renderer settling and
retirement perform any required model/history reconstruction on subsequent frames.

Cancel, completion, menu close, NR off, timing off, route/device/output/render extent change,
unsupported conditions and device loss clear the override. A recording already in progress
finishes with its captured settings; cancellation cannot undo submitted GPU commands. A menu
frame heartbeat handles close/off even without subsequent NR Dispatch. Device teardown also
cancels. Wrapped swapchain resize success and bridge PresentHost release cancel immediately;
unwrapped output/render extent changes are caught before the next Dispatch consumes an override.
No advisor lock is held while entering renderer or timing locks. Timing collector may
hold timing then advisor; renderer may hold renderer then advisor, never the reverse.

Idle contract: no sampling, clock reads, heap allocation, Config writes or locks for the advisor
on the frame path. Only an atomic active gate precedes the unchanged Dispatch. Existing timing
adds zero-tag fields; the accepted-sample callback is gated on a nonzero trial token.

## Menu and explicit application

One Start action when admitted, target FPS, settling/sampling words and counts, Cancel,
results including refusal reasons, explicit Apply recommendation and Copy report. No Start
on unsupported routes. Apply changes only the chosen scale/cadence via the ordinary menu
settings path, after all overrides are cleared; it does not save INI. Applying an old result
requires the same admitted route/context and a fresh timing contract matching the original
baseline after restoration. Resize/device-loss notifications invalidate completed results too.
English and pt-BR are authored here; other packs
may carry English fallback entries until native review. Never imply visual recommendation.

## Required game acceptance (not performed by host suite)

1. D3D12 after-upscale and Present, D3D11 D3D12 bridge, Proton RTX 4090 and Windows RTX 3060:
   verify eligible Start, settle/rebuild, 60 confirmed samples, reported cost against timing logs.
2. Cancel during settling and sampling; close menu; turn NR/timing off; resize; change route;
   remove device. Verify immediate override removal and subsequent normal history rebuild,
   no freed in-flight resources, no stuck state, exact pretrial Config and INI values.
3. Save during a trial and terminate the process: next launch retains original settings.
4. Refuse cadence through runtime prerequisites and verify its actual reason. Force missing
   timing and stale confirmations; no invented timing or recommendation after timeout.
5. Keep camera/scene repeatable, compare measured base FPS, exercise target above/below all
   candidates. Visually assess every applied recommendation separately, including moving HUD.
6. Vulkan, proxy, pre-upscale, RR, FG, capture and native D3D11 scene show explanation only.
   Idle advisor preserves default behavior; compare timing disabled/enabled separately.
