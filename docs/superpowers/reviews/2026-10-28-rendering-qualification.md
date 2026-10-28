# Rendering qualification — 2026-10-28

Status: integrated into `dlss-neural-rendering`; functional runtime qualification PASS; independent evidence audit PASS; post-integration strict host tier 94 PASS with no skips; source and runtime acceptance reviews approved with no open findings.

## Scope and source identity

This round implements the accepted supervised graphical preflight, bounded replay evidence retention, opt-in temporal replay corpus and permanent Vulkan `WorkingScale=1.25` route regression. It changes test infrastructure and documentation. Production renderer code, shaders, shipped INI defaults and numerical acceptance thresholds are unchanged.

- Base: `f8d9859aedff628bb0d65429a7a55b360294787a`.
- Qualified source: `abad97040570a2849a50ca2b80e49b78c2583e12`.
- Execution branch: `fix/rendering-qualification-20261028`.
- Execution checkout: `qualification-20261028` in the local worktree cache.
- Qualified aggregate invocation: `56e8495dd2bb478fa97e2bb425d3664b`.
- Qualified aggregate: `x64/logs/qualification-final.json`.
- Independent audit: `x64/logs/qualification-audit.json`.

All artifact paths below are relative to the execution checkout unless explicitly identified as the integration checkout. Final raw captures and retained evidence remain there; they are not committed to git. The second fixture run replaced its standard raw paths; the first invocation's retained bundle and audit remain preserved.

## Implemented behavior

### Supervised graphical preflight

The full rendering entrypoint checks its inherited X11 display with `XOpenDisplay` and `XCloseDisplay` before any host, compiler or GPU step. The connection attempt has an eight-second supervised deadline and bounded owned-process cleanup. Its receipt is versioned and bound to the invocation; it explicitly does not count as rendering coverage. Host-only mode omits the graphical preflight.

The real negative CLI check removed `DISPLAY` and `WAYLAND_DISPLAY` only from its own process environment. It exited 1, reported preflight FAIL after 0.040422473 seconds, left all six later steps NOT_RUN and recorded `runtime_coverage=false`. The requested temporal profile survived in that failure report. The first full-run preflight connected to the inherited session and passed in 0.040477300 seconds; the final full-run preflight passed in 0.040492685 seconds.

### Bounded retained judge evidence

The consolidated runner retains the selected replay profile's calibration, explicit policy, corpus metadata, optional moving plan and each reported judged variant's result, per-frame data, contact sheet and difference heatmap. It inventories evidence before execution, rejects unchanged stale inputs, checks regular files through no-follow directory descriptors, streams bounded copies and hashes, and verifies the published destination's identity and bytes. A destination-change failure stops collection, so rejected copies cannot accumulate outside the total byte budget.

Limits are 32 variants, 132 files, 16 MiB per artifact, 2 MiB for summary JSON and 128 MiB total. Retention failure prevents aggregate PASS without erasing the original functional or numerical result. Legacy fixture logs remain separately retained. The consolidated path requires judge evidence; the standalone legacy controls remain available.

### Explicit temporal replay and numerical policy

`--replay-quality` selects `temporal-v1`: 480 frames at a logical 60 Hz, with 80 warmup frames followed by one continuous 400-frame scoring suffix. Its artifacts use `tests/dlssnr-loopback/artifacts/replay-quality-run`; legacy replay keeps its own directory and 256/160-frame, 120 Hz behavior. The temporal step has a 5,400-second deadline; the legacy step keeps 3,600 seconds.

| Zero-based ordinals | Phase | Pose behavior |
| --- | --- | --- |
| 0–79 | Warmup | Hold; initial reset |
| 80–159 | Steady | Hold |
| 160–239 | Motion | Advance two pose steps per frame |
| 240–319 | Recovery | Hold |
| 320–359 | Fast motion | Advance twelve pose steps per frame |
| 360 | Cut | Distinct pose; current and previous pose equal; reset |
| 361–479 | Post-cut | Hold |

Capture IDs are one-based, so reset IDs are 1 and 361. The cut remains scored; no new warmup removes it. Material animation freezes with held poses, while jitter follows the rendered ordinal. The bounded plan parser and producer metadata checks reject contradictory profile, scoring, frame ID, reset and timestamp evidence. Temporal replay rejects custom captures, conflicting frame/warmup overrides and the incompatible legacy recorder check.

The explicit v1 policy preserves minimum coverage 0.25, pixel tolerance 0.01 and zero for all eight error floors. Calibration and judgment consume the same bytes. Five distinct A/A repeats define the measured bounds. Functional seam results, numerical candidate verdicts and unassessed perceptual quality are separate fields; the every-b A/A control must pass.

### Permanent Vulkan enlarged working-scale route

`nr-route-scale125` runs the full eleven-phase route fixture with only `WorkingScale=1.25` added to its case configuration. Model application remains enabled. Its required evidence is the actual 1280×720 after-upscale route, three 1600×900 working builds across three ownership periods, active evaluation counts 30/30/10, exact readback counts 28/28/8 and the existing cold, handback, ownership and lifecycle checks.

Phase counts must be physically consistent and MAE finite and nonnegative. Active host validation must remain zero. Model-side validation permits only attributed `VUID-vkCmdDraw-None-09600`, bounded by actual feature builds: the route cases require three present-host and three route builds, hence at most six messages; ordinary `nr` keeps its present-host-build bound. Missing, duplicate or malformed attribution fails, and 08795 fails even when attributed to the model.

## Verification completed before terminal runtime result

| Check | Observed result |
| --- | --- |
| Strict baseline host tier | 93 PASS, 1 FAIL, zero skipped; existing qualification documentation contained two literal home paths |
| Existing repository privacy guard after targeted repair | PASS |
| Combined rendering-orchestration suite | 107 tests PASS in 8.598 seconds, zero skipped |
| Modified Python compilation | PASS |
| Changed C++ harness formatting | clang-format 22.1.8 |
| Actual missing-display CLI | Expected FAIL in about 40 ms; later steps not run |
| Actual inherited-display preflight | PASS in about 40 ms |
| Whole-branch source review | Approved; zero open Critical, Important or Minor findings; no deferred findings |

The original baseline failure remains recorded above. The repair removed only the two private literal paths from the prior qualification document; it did not weaken the guard.

## Initial attempt and rebuild

The first full invocation, `0c50a0d0a2cc4475bde3b40308cfa111`, ran from 2026-10-28T21:26:31.999425Z to 21:47:38.285871Z and ended FAIL. Graphical preflight, all 94 strict host suites, FG inputs, moving NR and temporal replay passed. The Vulkan fixture stopped after 0.060605 seconds before compilation/GPU execution because the preserved DLL timestamp preceded production files freshly checked out in this worktree. FG provider remained NOT_RUN. Its aggregate is retained as `x64/logs/qualification-full.json`; no result was relabeled.

The initial replay took 1,041.041857 seconds and retained 20 judge artifacts totaling 1,968,019 bytes. Every enabled variant covered all 400 scored frames and completed the model at cut ID 361. The every-b A/A control passed; cadence2, cadence4 and adaptive4 retained numerical FAIL. `x64/logs/qualification-initial-audit.json` independently checked 115 distinct files, the retained hashes/byte counts and four raw guide samples. That audit remains FAIL because Vulkan and provider did not qualify. Its issues were confined to the unsuccessful/missing stages and aggregate coverage.

Source comparison from the original binary source commit `d845b511e1b380ba2cf83d1e8635bbd62502ef16` to the candidate confirmed no production source diff. The original binary pair was:

| Binary | Initial SHA-256 |
| --- | --- |
| OptiScaler.dll | `3f6a2abc1ba497c52b469949ca513af52d716083d4c2d2278d0e4ead2e9311f2` |
| nvngx.dll_dlssnr.dll | `0ffc8e8902148b10db8555d6ecd76d772ee2343930df32e4aac57e5b2319d011` |

The recovery was a real supported `./build-local.sh Release`, which completed with exit 0 and produced both outputs. No timestamps or fixture guards were altered. The build log is `x64/logs/qualification-rebuild.log`; stage logs are under `x64/logs/20261028_185024-612871-c6608e7c/`. Compilation included the production PCH and sources, with full link-time compilation. Existing MESA initialization messages and linker warning LNK4098 are visible in the retained log; this is not a warning-free-build claim. A direct read-only call to the fixture's existing DLL marker/freshness checker then passed. The checkout remained clean at the reviewed candidate commit.

## Qualified runtime command and binary identity

```sh
env DISPLAY=:0 WAYLAND_DISPLAY=wayland-1 XDG_SESSION_TYPE=wayland \
  tools/test-rendering.sh --no-build --replay-quality --fg-steady-seconds 15 \
  --report x64/logs/qualification-final.json --log-dir x64/logs/qualification-final
```

The fresh run started at 2026-10-28T21:53:05.859728Z from the clean candidate checkout, using the newly built production pair. The explicit `--no-build` selects that completed build; harnesses compile through their supported fixtures.

| Binary | SHA-256 |
| --- | --- |
| OptiScaler.dll | `4d48c575e880049ca56dcc6a75d27d88be842621a10a5c66c0e6e820e31a805c` |
| nvngx.dll_dlssnr.dll | `5bd7d21ff0e9c814d41c9ad6e85ffaa144be53e18573e08e922ee9bc5924d1c8` |

The full run ended at 2026-10-28T22:17:47.356165Z, exit 0, after 24 minutes 41.496 seconds. Its aggregate is PASS with `runtime_coverage=true` and no step findings. Recorded replay environment: NVIDIA GeForce RTX 4090, driver 615.78.08, Proton - Experimental, model SHA-256 `6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927`.

| Step | Status | Wall time, seconds |
| --- | --- | ---: |
| graphics-preflight | PASS | 0.040 |
| host | PASS: 94 suites, no failures or skips | 158.727 |
| fg-inputs | PASS, including expected failing mutation controls | 9.089 |
| nr-moving | PASS | 47.481 |
| nr-replay | Functional PASS; numerical results below | 1028.191 |
| vulkan | PASS: all five required cases | 185.839 |
| fg-provider | Runtime and counter qualification PASS | 51.515 |

The production pair's hashes matched before and after runtime and were independently rechecked afterward. The source checkout remained clean at `abad9704` through qualification.

## Independent evidence audit

The reviewed one-off helper exited 0 with PASS and an empty issue list at 2026-10-28T22:17:58.881650Z. It checked 140 distinct files: the aggregate, seven receipt snapshots, 86 retained logs, 20 retained judge artifacts, four source contracts, fourteen raw metadata files and eight sampled raw guide planes. Repeated identity/content checks made 170 file reads in total. This does not hash every raw output image or independently rerun the image metrics.

A separate final runtime-acceptance review accepted the evidence within this fixture scope, with zero open Critical, Important or Minor findings. It specifically retained the numerical, sampled-guide and hardware coverage limits.

The temporal bundle contains all 20 expected judge artifacts for four judged variants, totaling **1,967,934 bytes**, within the configured bounds. Saved receipts match embedded evidence, versioned suite IDs match their current supervisor invocations, and current corpus, policy, plan, calibration and result identities agree. Post-hoc inspection does not reconstruct the pre-run freshness inventory; that gate is the runner's recorded responsibility.

| Evidence | SHA-256 |
| --- | --- |
| Final aggregate | `9d1a37617a59d71370e23cfebbb2a8954dcf7327476fc3f291a88d7d1d660ec0` |
| Independent audit | `2ea97c35d658cc20defcd7452ee48da070ccc34fe244d573465701b1824b0f64` |
| Final capture manifest | `f17fea2bac406cb0821d98225f720920240d48e45e0a30b40fe9185ed1497c57` |
| Explicit v1 policy | `e92a29e5025900900837557ad1c5b18dc0d412b61b98ff2c8feba26cbdd7dd6c` |
| Moving plan | `03f4e5ec2feb4b95bfc416ab2b09d0af71e2743b7a1f8eb9564288bfc064bf97` |

The additional `x64/logs/qualification-replay-comparison.json` records exact equality between the two runs' eleven per-phase decision summaries and four judged variants' metrics, counts, bounds, policy, pulse groups, verdicts, reasons and environment. GPU timings were excluded from this comparison. All 20 first-run retained artifacts still matched their original hashes and total 1,968,019 bytes after the second fixture execution replaced the raw paths. This establishes the observed repeatability and retained-copy independence for these two executions on this machine.

## Temporal measurements and numerical interpretation

All eleven raw runs completed 480 frames. The ten enabled runs had zero skips in the 400 scored frames, completed their requested model work and executed a successful complete model at scored cut ID 361. The disabled control preserved all 480 input images by hash. Every-a and every-b executed the model on all 400 scored frames, made identical decisions and produced 480/480 bitwise-identical output frames across their fresh processes. The five separate A/A calibration trials qualified, and every-b passed the calibrated judge.

Each decision cell below is **model / carried / skipped**. `carried` is a reuse decision. Ordinals are zero-based; capture IDs are ordinal + 1. Startup skips remain explicit and do not reappear at the scored cut.

| Phase | Frames | every-b | cadence2 | cadence4 | adaptive4 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Warmup, unscored | 80 | 49 / 0 / 31 | 25 / 24 / 31 | 13 / 36 / 31 | 19 / 0 / 61 |
| Steady | 80 | 80 / 0 / 0 | 40 / 40 / 0 | 20 / 60 / 0 | 20 / 60 / 0 |
| Motion | 80 | 80 / 0 / 0 | 40 / 40 / 0 | 20 / 60 / 0 | 80 / 0 / 0 |
| Recovery | 80 | 80 / 0 / 0 | 40 / 40 / 0 | 20 / 60 / 0 | 34 / 46 / 0 |
| Fast motion | 40 | 40 / 0 / 0 | 20 / 20 / 0 | 10 / 30 / 0 | 39 / 1 / 0 |
| Scored cut | 1 | 1 / 0 / 0 | 1 / 0 / 0 | 1 / 0 / 0 | 1 / 0 / 0 |
| Post-cut recovery | 119 | 119 / 0 / 0 | 59 / 60 / 0 | 29 / 90 / 0 | 44 / 75 / 0 |
| **Scored total** | **400** | **400 / 0 / 0** | **200 / 200 / 0** | **100 / 300 / 0** | **218 / 182 / 0** |

Adaptive4 executed the model on 119/120 moving frames and the cut, and reused output on 181/279 frames in held-pose phases. Its scored interval histogram was 158 frames at interval 1 and 242 at interval 4. These are replay decisions, not a measurement of game FPS improvement.

The numerical results below are the retained judge's full-frame `all` region against every-a. They remain in the judge's numerical scale, without perceptual interpretation.

| Variant | Verdict | golden_mae | golden_bad_fraction | detail_error | disocclusion_mae | pulse_p95 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| every-b | PASS | 0 | 0 | 0 | 0 | 0.006252285799 |
| cadence2 | FAIL | 0.0005172003775 | 0.0176108724 | 0.02105428632 | 0.0009420385009 | 0.007482321615 |
| cadence4 | FAIL | 0.0007969897475 | 0.02407274306 | 0.03872551837 | 0.001039651526 | 0.007244988492 |
| adaptive4 | FAIL | 0.0002879586591 | 0.008080805122 | 0.01194657096 | 0.0001417014641 | 0.007658953612 |

All three cadence candidates exceed the measured A/A envelope. In this run the listed spatial-error bounds were zero, while pulse and temporal bounds were nonzero. `pixel_tolerance=0.01` is not a blanket acceptance threshold for every metric. Adaptive4 is closer to the reference in several aggregate spatial metrics than both fixed candidates, but has the largest full-frame pulse p95 of the three. Functional PASS, numerical FAIL and perceptual NOT_ASSESSED are therefore preserved separately.

The next useful quality investigation is an inference from these measurements: align the retained per-frame metrics with phase boundaries, model/reuse decisions and time since the last model execution to locate adaptive4's excess pulse. The current aggregate does not establish which transition causes it. These measurements do not justify changing defaults or widening tolerances.

### Sampled raw guide oracle

The existing GPU-guide oracle checked four moving frames, reading hash-matched f32 depth and f16 motion planes. It was not applied to held poses or the cut because that oracle expects a moving fixture.

| Ordinal / capture ID | Samples | Maximum error, pixels | Mean motion, pixels | Status |
| --- | ---: | ---: | ---: | --- |
| 160 / 161 | 1445 | 0.001694066 | 0.310962695 | PASS |
| 239 / 240 | 1436 | 0.001361131 | 0.294020982 | PASS |
| 320 / 321 | 1444 | 0.003058282 | 1.762570523 | PASS |
| 359 / 360 | 1383 | 0.002151728 | 1.636229183 | PASS |

## Vulkan runtime results

All five cases passed with active host validation and zero host validation errors. The independent audit checked the retained raw sidecars and build logs. The admitted model VUID remained visible and bounded:

| Case | Status | Host errors | Model messages | Observed build bound |
| --- | --- | ---: | ---: | ---: |
| nr | PASS | 0 | 2 | 2 |
| nr-route-quiet | PASS | 0 | 6 | 6 |
| nr-route-quiet-wdx12 | PASS | 0 | 0 | 6 |
| nr-route-guides | PASS | 0 | 6 | 6 |
| nr-route-scale125 | PASS | 0 | 6 | 6 |

Every nonzero model-message attribution was exactly `VUID-vkCmdDraw-None-09600`. The audit found no forbidden 08795 in the checked sidecars and retained case logs. This is not a universal validation-clean claim.

For scale125, three ownership periods each showed claim, enlarged feature build and release. All three route builds were `1600x900 (frame 1280x720)`. The active phase counters were 30/30/10 successful harness upscaler evaluations and 28/28/8 exact readbacks; MAE was zero for those checked readbacks. The route showed no box-enlarge fallback or failed-route marker. Both handbacks occurred after the required ten-second quiet period, with recorded drains of 95 and 94 ms. These checks retain the stated up-leg and lifecycle coverage limits.

## Frame-generation provider result

The real production-provider fixture passed runtime regression and its requested x2 counter qualification. It completed three active phases with at least 15 seconds of steady duration each, plus the disabled control and resize. All nine complete windows, three per active phase, had the exact requested 2:1 integer ratio.

| Phase | Steady duration, seconds | Application presents | Plugin actual presents | Ratio |
| --- | ---: | ---: | ---: | ---: |
| 0, active | 15.024339 | 714 | 1428 | 2.0 |
| 1, disabled control | 0.237014 | 16 | 16 | 1.0 |
| 2, active | 15.054069 | 648 | 1296 | 2.0 |
| 3, active | 15.047304 | 520 | 1040 | 2.0 |

The disabled phase intentionally has no requested fifteen-second window. The requested configuration was one generated frame, and the provider's recorded capability range was one to one. Applied-option readback remains unknown (`effective_configured_generated_frames=null`): the GetState options parameter is an input, not a readback of the applied configuration. Binary staging remained unchanged; recorded source head was `abad9704` with `dirty=false`. This is the plugin's present-counter qualification in this fixture, not scanout capture, proof of unique generated images, a latency metric or approval of a hardware family. Other multipliers, native Windows and menu coverage remain untested by this run.

## Integration record

The primary checkout was clean on `dlss-neural-rendering` at `f8d9859aedff628bb0d65429a7a55b360294787a`, with the expected reflog. The qualification record was committed before integration in documentation-only commit `98ef2a7460dcb9636e9ea20c6cce4138558e1a85`; the already authorized fast-forward from `f8d9859a` to `98ef2a74` then succeeded. The implementation remains exactly the runtime-qualified `abad9704`; later changes in this round only record documentation and plan completion.

The strict host tier was run again in the primary checkout after that fast-forward:

```sh
python3 tests/run_all.py --tier host --jobs 4 --strict \
  --report x64/logs/qualification-integrated-host-20261028.json \
  --log-dir x64/logs/qualification-integrated-host-20261028
```

It exited 0 with **94 PASS, zero FAIL, SKIP, TIMEOUT, ABORTED or INCONCLUSIVE**, and its selected/results lists matched the current host registry exactly. Run `5005e5beb24c40e5b941110956b06997` recorded head `98ef2a7460dcb9636e9ea20c6cce4138558e1a85`, `dirty=false`, start 2026-10-28T22:23:22.509949Z and finish 22:26:12.654999Z. The report in the primary checkout has SHA-256 `48b9c95a39a36fe0fef53eb5a8f1d7e53b352314eb4dd4ff40edaf4c4c314219`.

This final documentation update records the completed integration and host check. It introduces no implementation change after the qualified source. The execution checkout and its ignored evidence remain available; no git push or game deployment was performed.

## Review resolutions and execution decisions

- A retained-copy race initially allowed a hash to describe bytes detached from the reported pathname. Publication now rechecks the named destination, file identity and contents.
- A follow-up review found rejected published copies could accumulate outside the retained-byte accounting. Collection now stops on destination change; reduced-budget controls include the rejected bytes within the physical limit.
- Temporal replay initially allowed an incompatible legacy recorder check and contradictory corpus summaries. Both are rejected before their evidence can qualify.
- Vulkan route evidence initially allowed missing VUID attribution and impossible phase counts. Both are rejected by strict host controls.
- Independent review of the one-off audit helper also closed three findings: only current reported raw variants are selected, inputs/provider receipts must match their own supervisor run IDs, and Vulkan message counts are independently bounded by verified feature-build logs. All three scoped fixes were accepted with no remaining findings. The helper's SHA-256 is `115d4dced564ccbd8142c9ee1eda3faad51e76931d08228ce884dae430b40eb0`; its diagnostic audit of the initial run correctly remained FAIL.
- Task changes were developed in disjoint local snapshots and applied to the canonical isolated checkout by one remote executor. Compiler, Wine and GPU work stayed serial.
- The initial decision reused the supplied qualified production pair because production sources were unchanged. The real Vulkan timestamp guard superseded that decision: the supported Release build produced a new pair, and the entire qualification was restarted with those exact hashes. The initial failure report and diagnostic audit remain retained. No model download or game deployment was introduced.
- The immutable source review ran concurrently with the serial qualification. Runtime acceptance remained a separate gate.
- The execution checkout is retained with its raw captures and bounded invocation bundle for inspection. No destructive cleanup is part of this round.
- The authorized local integration used fast-forward after checking the primary branch's actual HEAD, status and reflog. No git push or game deployment was performed.

## Coverage boundaries

This is qualification infrastructure and evidence, not a claim of improved image quality or frame rate. Candidate numerical FAIL is retained as a measured result. Perceptual quality is not assessed. The Vulkan evidence confirms the enlarged working route and existing readback/lifecycle contracts; it is not an independent proof of down-leg pixel correctness, exposure readback or command-buffer resubmission. The 70 Vulkan harness upscale evaluations are not a count of independently proven NR dispatches. Provider interpolation ratios are fixture telemetry, not a game performance benchmark. The full moving-scene fixture exercises the existing no-plan harness path; no separate full legacy numerical replay is claimed.
