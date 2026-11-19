# In-game NR cost advisor

2026-11-19, round 2. Inspired by the idea of a reversible in-game trial in NeuRotic;
no code, tables or translations are taken from that project. Cost measurement is not image-quality
assessment. This implementation has host contracts only; game qualification remains open.

## Trial settings and routes

| Setting | D3D12 after SR / after RR / Present / D3D11 D3D12 bridge | Native Vulkan |
|---|---|---|
| WorkingScale / RRWorkingScale | Dispatch resolves the scale every call and settles extent changes before reconstruction. Trial overrides only that call's resolved scale. After RR uses `RRWorkingScale` and retains `RRPasses`; other paths use `WorkingScale` and retain their existing pass snapshot. Apply writes only the appropriate key; the table/export name it. | Runtime resolves scale, but no matching fence-confirmed timing stream: no advisor section. |
| Cadence | A local requested-cadence override feeds the unchanged runtime scheduler. Its refusals remain authoritative, including FG unless `CadenceWithFrameGen` allows it. Refused candidates time out with that runtime reason, not measurements of the fallback cadence. | No matching timing/cadence path: no advisor section. |
| Passes | Not trialled: legacy-to-tracked migration can change the session's ownership mode permanently. Restoring a number would not undo that migration. Already tracked sessions also retain their count/overrides in this version. | Single master pass; no trial. |
| Stage 0/1 | Not trialled: placement is selected before Dispatch, outside the isolated override seam. A safe trial would require a placement transaction across those callsites. Stage 0 is required, including the after-RR route. | Stage changes need a restart, not trialled. |

Source anchors in base `6972e2d7`: `retoque/shaders/dlssnr/DlssNr_Dx12.cpp:1312` selects
RR versus regular scale, `:1365-1400` settles reconstruction, `:1905-2019` admits and schedules
cadence. `DlssNr_Dx12_Lifetime.cpp:300-357` migrates legacy ownership. Native Vulkan resolves
scale at `retoque/dlssnr/DlssNrFeature_Vk.cpp:1790` and refuses placement changes after Post
claimed the process at `:2878`. Advisor never releases or changes ownership of GPU resources.

## One admission policy

`DlssNr_AdvisorPolicy.h::CheckConfig` is the Config-only definition, shared by Dispatch,
RenderAdvisor and AdvisorMenuTick. NR off, native Vulkan, driver proxy or startup native-D3D11
scene transport hide the section entirely. Stage 1, adaptive cadence, peripheral compression,
hold-frame and timing preparation each have one prioritized explanation. The existing timing
keys can be enabled/set to interval 1 by explicit menu clicks; the trial itself changes neither.

Per-frame extras belong at the callsite: `colour == output` (in-place after-upscaler/Present
composition), no native-D3D11 scene frame, no active capture, visible menu, and stable route/device/
render/output extent. The menu uses a fresh confirmed after/after-rr sample, nonzero evaluation
identity and valid resolved scale matching that sample as its readiness evidence. The renderer
rechecks before using the override; stale UI evidence cannot bypass the actual frame's gates.
RR and FG are not Config-policy exclusions. RR placement is part of the context, so switching it
aborts even when both routes happen to resolve to the same scale.

The upstream menu file adds only an include and `DlssNr::AdvisorMenuTick(config, visible)`.
The helper in the fork module handles close/off/timing/route changes even when another tab is open.

## Base-frame clock, including FG

Generated presents are not NR evaluations. The adapter allocates an evaluation ordinal at every
attempted Dispatch while a trial is active; it measures entry-to-entry elapsed time only between
adjacent NR evaluation ordinals of the same candidate. Duplicate/out-of-order identities and gaps
produce no interval. An evaluation scope invalidates the preceding clock on every unsuccessful
exit, including refusals after Frame() and exits without reportSkip. This preserves a gap rather
than turning skipped work or a restart into a cheap base frame. No new counter/clock runs while idle.

Thus generated presents between two real NR evaluations no longer discard the measurement.
This remains observed pacing at the NR seam, not display FPS or GPU critical-path time. Multiple
views, skipped evaluations, pool saturation and ambiguous submissions can make a window incomplete.

**Menu interlock:** `OptiScaler/hooks/Streamline_Hooks.cpp:2400` forces the sent Streamline DLSS-G
mode to `eOff` while the menu is visible and retains resources. The resulting mode is stored in
`state.dlssgLastSetMode` at `:1793`; `menu_common.cpp:2574` resends options on visibility changes.
`DlssNr_Dx12_Lifetime.cpp:81` returns FG active for an active/unpaused managed provider, a nonzero
recent NGX interpolation count, or a non-Off Streamline mode. For Streamline alone, after the
interlock and expiry of any NGX observation (500 ms in `State.h`), it therefore returns false.
Managed/NGX FG can still make it true. The advisor neither bypasses this interlock nor changes
`CadenceWithFrameGen`. A cadence measured while FG is paused may be refused when FG resumes.
The action and copied report explicitly say FG may be paused; results do not qualify closed-menu
FG throughput, pacing, or image quality.

## Sampling, statistics and recommendation

Pure `DlssNr_Advisor.h`: idle, settling, sampling, restoring, done, aborted. Unique candidate tokens
are stamped on recorded GPU metadata. The production collector feeds every accepted sample only
after its existing fence/recording certificate. It passes `carried` too; no-model evaluations are
not mislabelled as model runs. A recording-ID watermark rejects late settling recordings; duplicate
IDs, old tokens, reset/capture frames, invalid timing and missing base intervals are excluded.

Settle for at least 1 second AND 30 confirmed samples, then collect 60. Timeout is 20 seconds per
candidate, absolute even during contract churn. Runtime refusals retain their own text. Once
sampling starts, a changed timing contract aborts the trial rather than mixing workloads.

Report GPU Dispatch **median and mean per base evaluation**, mean of the slowest ceil(N*0.05)
GPU samples, base-interval **median and mean**, the same slow-tail statistic of base intervals,
model/carried counts, and 100 * mean GPU / mean base interval. GPU/base are windows, not same-frame
pairs: an entry-to-entry interval describes the preceding evaluation. GPU overlap/queues can make
the fraction exceed 100%. Never subtract GPU time from CPU time to predict no-NR FPS.

For target F, budget B = 1000/F ms. Recommend the first preferred candidate with all of:

- 60 valid samples and no refusal;
- mean base interval <= B;
- mean of the slowest 5% base intervals <= 1.5 * B;
- at least ceil(60/cadence) observed model frames, preventing a carried-only/incomplete model window winning.

No qualifying candidate means no recommendation. Both mean and tail see cadence's long frames:
60 frames alternating one 50 ms model frame and three 5 ms carried frames have median 5 ms and
mean 16.25 ms, but tail 50 ms; this fails the 60 FPS target's 25 ms tail cap. Tests also cover
80/5 ms, whose mean fails, and a stable 16 ms cadence window that passes.

Preference order: original, lower 0.75/0.5 scales, cadence 2 at original scale, 0.25 scale, then
cadence 4. Skip choices that do not reduce the corresponding original knob. This tries moderate
spatial reductions first, avoids quarter-scale ahead of cadence 2, and leaves aggressive temporal
reuse last. It is a declared heuristic, not a measured ranking of image quality. No combined
knob trial, cost increase, or automatic application. Scene/motion/CPU/limiter changes require rerun.

## Restoration and off contract

Trials never write Config, including CustomOptional's backing state; Save Settings or a process
crash therefore cannot persist a trial value. Each Dispatch copies an override under the advisor
mutex, then executes without that lock. Restoration clears the override; ordinary settling,
history reset and resource retirement handle the next normal frames. Values including auto/unset
remain untouched. Only explicit Apply writes the chosen scale key/cadence, never saves the INI.

Cancel/completion, menu close, NR/timing off, incompatible settings, route/RR/device/extent change,
resize and device loss clear overrides. In-progress/submitted work finishes with its captured
values. Wrapped resize success and bridge PresentHost release also invalidate completed results.
Apply requires fresh original-baseline timing, unchanged context, and a still-valid result.
No advisor lock is held while entering renderer/timing locks; both may acquire advisor, never reverse.

Idle: atomic gates and an empty scope/decision, no advisor counter increments, sampling, clocks,
allocation, Config mutation or lock on the frame path. Timing's callback only runs for nonzero
tokens. No shader/cbuffer changes, new INI keys or advisor-owned GPU objects.

## Required game acceptance (not performed by host suites)

1. After SR and after RR, D3D12 Present, D3D11 bridge, Proton RTX 4090 and Windows RTX 3060:
   check eligibility, correct scale key and constant pass count, reconstruction and confirmed samples.
2. With Streamline DLSS-G and managed/NGX FG, observe the menu interlock and actual FG state; compare
   logged evaluation intervals to base frames despite generated presents. Check cadence refusal text
   with CadenceWithFrameGen off, and separately qualify allowing it. Do not bypass the menu interlock.
3. Repeat camera/scene; compare mean/tail and model/carried counts to logs; test targets above/below
   the measured windows. Visually assess applied choices separately, including moving HUD.
4. Cancel in settling/sampling; close menu; switch NR/timing off; resize, RR/route change, device loss.
   Verify no override on the next frame, correct normal history rebuild and no freed in-flight resources.
5. Save mid-trial and terminate: next launch keeps original settings. Stale confirmations/results
   must not be measured/applied to a new trial/device/route.
6. NR off, Vulkan and proxy show no advisor heading or explanation. Missing timing offers one recovery
   action; adaptive/hold/etc. show only the highest-priority actionable reason. Validate translated layout.
