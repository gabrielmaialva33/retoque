# GPU fixture determinism, 2026-11-20

This is a fixture investigation on RTX 4090, NVIDIA 620.73.04, Arch Linux and GE-Proton11-12-x86_64.
It changes no production runtime, shader, model or game installation. Native Windows, games,
image quality and frame-time qualification remain outside this evidence.

The initial Release build used production tree `675722dc`:

- OptiScaler DLL SHA-256: `906af5607569185a34f9b3f1561ccef62890df9e6e3787898b7b8f21f665bb5e`.
- NR model SHA-256: `6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927`.
- Forwarder SHA-256: `2ec1fe483c4ac7be929cab562c7deff16b5d186045dfd39429e5ada5cb9104ec`.

## Findings and source history

| Failure | Cause and boundary | Source history |
|---|---|---|
| Overlay instrumentation never starts | Exact anchor still named the old one-argument teardown definition. Match the current three-argument definition, still exactly once, at the body entry. All GPU counters and fault controls remain. | Signature changed in `c9a4500a`, 2026-11-09. |
| FG guards refuse before launch | Same-user capability-bearing `tailscaled` hides maps and executable links. Both FG fixtures refused every unreadable process except systemd/PAM. The shared guard accepts only narrow native system-service identities, checks argv/comm and exposed executable agreement, and requires a root-owned non-writable system binary. Unknown processes and Wine/game evidence remain refusals. | This is an environmental trigger for the existing fail-closed guard, not a rendering failure. |
| HDR half-size motion never becomes ready | Wayland initially configured the owned fixture dialog at 3440x1414. Serial validated presents plus CPU readbacks repeatedly exceed the estimator's 250 ms history-gap policy; a field requires six consecutive warm-up frames. Reapply the requested 632x446 client extent after initial configure. | The HDR fixture has relied on initial window sizing since `f6c99218`, 2026-10-15. First supplied failing observation: 2026-11-19. This does not establish when compositor/runtime timing changed. |
| Scale 1.25 gets 26/27 rather than 28 exact readbacks | The checker assumed each phase reuses exactly two images. WSI may acquire three/four; the first acquisition of each still contains the prior phase. Record every acquired index and require one exact readback per repeated index. | Fixed 28/28/8 expectation introduced by `2ad67dec`, 2026-10-28. The latent assumption is identified from source; no driver regression is claimed. |
| Cold `nr` / half-size case intermittently ends unchanged | Twelve presents can end before asynchronous model construction plus image reacquisition. The fixture now continues the same scene until eight edited readbacks, within 30 seconds, retaining all warm-up evidence and the original pixel/lifecycle gates. | Fixed twelve-present budget introduced by `aa122612`, 2026-10-14. |

The root-owned service/credential-agent identity fallback is an operational classification of ordinary workstation processes,
not an adversarial process-authentication boundary: Linux permits processes to rewrite argv/comm. Readable
maps always take precedence. For unreadable identities the fallback is deliberately narrow; it does not
accept every native-looking command line. Systemd/PAM and gpg-agent/ssh-agent exceptions predate this change; the latter now require the
additional native identity evidence too.

## Controlled observations

The original harness on the initial DLL reproduced all three HDR motion failures. It requested 32 presents
per profile at 3440x1414; the three HDR chains each evaluated 28 frames and synthesized none. Its final
517x389 chain synthesized 21 frames. With only the fixture sizing/readiness/accounting changes, the same
DLL and driver passed all four selected cases. At 632x446 the HDR chains synthesized 22, 19 and 19 frames;
the odd-sized chain still synthesized 21. Original alpha and core/synchronization validation gates passed.
The model's existing, narrowly attributed `VUID-vkCmdDraw-None-09600` allowance is unchanged.

The unmodified twelve-present executable was also run with the existing test layer delaying off-thread
builder calls (`RETOQUE_SLOW_OFF_DEVICE_THREAD_MS=100`, total 1500 ms per burst). Both active-model cases
failed with unchanged readbacks while construction was still pending, reproducing the reported failure
without clearing driver caches or changing the DLL. This establishes a fixture/build readiness race;
it does not identify cold cache contents as the sole source of build latency. With the same explicit
layer delay, the fixed fixture passed both cases (84/73 presents, 40/41 carried while building,
44/32 evaluations, eight or more edited readbacks per SDR chain). A separate `ApplyModel=false`
control with readiness still requested failed with `model edit readiness timed out`, proving that
a scene which never receives an edit is not accepted.

The scale oracle still requires 30/30/10 successful evaluates and presents, three 1600x900 route builds
for 1280x720 output, all eleven phases, correct ownership/hand-back, and clean host validation. It now
requires the complete acquisition sequence and actual swapchain image count. Host rejection controls
cover malformed/missing sequences, out-of-range indices and a missing readback for two-, three- and
four-image acquisition patterns. Exact presentation pixels establish host stand-down, not the off-screen
route's scaling quality.

## Reproduction and retained evidence

From the isolated worktree, with prerequisites documented in the individual suite READMEs:

```sh
./build-local.sh Release > x64/logs/gpu-suites-build-initial.log 2>&1
flock -n -o "$XDG_RUNTIME_DIR/msvc-prefix.lock" python3 tests/vulkan-present-host/run.py \
  --only nr-hdr-half-synth,nr-route-scale125,nr,nr-working-scale
flock -n -o "$XDG_RUNTIME_DIR/msvc-prefix.lock" python3 x64/logs/cold-fixed-control.py
```

Ignored evidence under `x64/logs/vk-original`, `vk-cold-original` and `vk-fixed` retains the actual
application results, production logs and validation messages. `gpu-suites-vk-baseline.log` and
`gpu-suites-vk-fixed.log` retain the original/fixed judgments. The delayed controls load the runner as a Python module and set both cases' `NR_TIMING` knobs to
`RETOQUE_SLOW_OFF_DEVICE_THREAD_MS=100` and `RETOQUE_SLOW_OFF_DEVICE_THREAD_TOTAL_MS=1500`, which also enables
the timing layer. Merely exporting the variables does not enable that layer and is not a delayed control.
The fixed control keeps `--min-changed 8`; the original control uses its retained original executable and
omits that new argument. An initial control attempt
which accidentally passed that new argument to the old executable failed argument parsing and is not
counted as a rendering reproduction (`gpu-suites-cold-negative.log`).

Whole host/Wine tier runs are separate gates; a focused pass above does not imply either tier passed.
Their aggregate reports retain the commit, dirty state, per-suite statuses and selected binary identity.
