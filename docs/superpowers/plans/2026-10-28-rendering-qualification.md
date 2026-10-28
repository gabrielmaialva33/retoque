# Rendering Qualification Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development for the disjoint tasks below. Root integrates and runs remote verification serially.

**Goal:** Implement the accepted four-item qualification round with reproducible evidence.

**Architecture:** Extend the existing rendering executor, offline replay producer and Vulkan route fixture. Keep production behavior unchanged and retain original numerical verdicts.

**Tech Stack:** Python stdlib, existing C++ test harness, X11 library, existing MSVC/Wine/Proton/Vulkan tooling.

**Spec:** docs/superpowers/specs/2026-10-28-rendering-qualification.md

## Global Constraints

- No production renderer/shader/INI changes, git push, model download or game deployment.
- Root alone uses Winx; agents own disjoint local snapshot files.
- Remote compiler/Wine/GPU work runs serially.
- Short replay stays 256/160 at 120 Hz; temporal-v1 is opt-in 480/80 at 60 Hz.
- All numerical policy floors remain zero; no inferred perceptual approval.
- Model VUID allowlist stays exactly VUID-vkCmdDraw-None-09600; 08795 is an error.
- Follow the spec's retention bounds, freshness and receipt identities.

## Review Focus

- A present but unusable DISPLAY must fail without later subprocesses.
- Old/symlinked/oversized judge files must never become current retained evidence.
- Static phases must freeze both pose and animated material, with correct previous-frame guides.
- A scored cut must preserve reset and complete real coverage, not erase an inconvenient frame.
- WorkingScale evidence must name the actual after-upscale route, not a cold present-host build.

## Task 1: Graphical preflight and retained replay evidence

**Files:** tools/test-rendering.py; new tools/graphics_preflight.py; tests/rendering-orchestration/test_orchestration.py; new test_graphics_preflight.py in that suite.

**Interfaces:** Add a preflight Step before the complete plan, omit for host-only. Add optional replay artifact_root on Step and inventory/retention helpers. Existing run_step and result identities remain the authority. Root later adds --replay-quality wiring and VK_CASES.

- [ ] Add tests for connection refusal, exact inherited display, close, fail-fast plan, stale/malformed receipt, timeout and interruption.
- [ ] Run new tests and record the intended RED.
- [ ] Implement the 8-second supervised X11 preflight and explicit non-runtime receipt.
- [ ] Add retention tests for fresh bytes/hash, later overwrite, old unchanged files, identical rewrites, symlink/file-special/size/count refusals and functional-vs-quality verdicts.
- [ ] Record RED, implement bounded allowlist retention, then run the complete orchestration suite GREEN.
- [ ] Review diff and commit this task after remote verification.

## Task 2: Versioned temporal corpus and explicit policy

**Files:** tests/dlssnr-loopback/run.py, run_replay.py, moving_scene.h, harness.cpp; new replay_quality.py, replay-quality-policy-v1.json, moving-plan parser header if useful; new host test under tests/rendering-orchestration.

**Interfaces:** --replay-quality selects temporal-v1 and artifacts/replay-quality-run. run_replay.run accepts the selected profile while preserving the default call. Emit judge/policy.json and judge/corpus.json; calibrate and judge receive the identical policy. Metadata and phase decisions are additive to replay-report.json.

- [ ] Test the exact spec phase ranges, timing, monotonic ids, single scoring suffix, hold/material identity, previous-pose continuity and cut resets.
- [ ] Test contradictory overrides/captures, malformed plans and mismatched producer metadata are rejected.
- [ ] Record RED; implement deterministic stdlib plan generation and the bounded portable C++ plan reader.
- [ ] Extend only the test harness path to consume the plan, preserving the no-plan path.
- [ ] Test explicit policy reaches calibration/judge unchanged; report separates seam, numerical and unassessed perceptual results.
- [ ] Run host tests GREEN; format C++ with the repository's clang-format 22 pin.
- [ ] Root compiles and runs the actual temporal profile serially, records cut behavior, complete coverage and original visual verdicts.
- [ ] Review and commit.

## Task 3: Registered Vulkan scale 1.25 route

**Files:** tests/vulkan-present-host/run.py; new tests/rendering-orchestration/test_vulkan_scale.py.

**Interfaces:** nr-route-scale125 is a ROUTE_CASES member using --route; check_route keeps existing eleven-phase gates and calls a scale-evidence helper for this case. Root adds its name to consolidated VK_CASES.

- [ ] Use the preserved manual report/log and source to pin route build text and expected working dimensions.
- [ ] Add behavioral cases rejecting wrong/missing scale/dimensions, missing eval/build, fallback, invalid validation and 08795 even misattributed as model.
- [ ] Record RED; implement case staging plus strict evidence checks without weakening quiet-route checks.
- [ ] Run host checks GREEN.
- [ ] Root executes the real registered case and retains original result/validation/OptiScaler logs.
- [ ] Review and commit.

## Task 4: Integration, documentation and qualification

**Files:** tools/test-rendering.py integration; tools/README.md; tests/README.md; loopback and Vulkan READMEs; docs/superpowers/reviews/2026-10-28-rendering-qualification.md.

- [ ] Verify clean host baseline in the isolated worktree with existing pinned dependencies.
- [ ] Integrate --replay-quality selection, longer opt-in timeout and nr-route-scale125 required case.
- [ ] Check source diff and compile all modified Python, then run the strict full host tier.
- [ ] Verify missing DISPLAY fails promptly and host-only is unaffected.
- [ ] Run the complete serial rendering qualification using the qualified unchanged DLL pair, including the temporal profile and new Vulkan case.
- [ ] Independently verify bundle byte counts/hashes and retained visual metadata.
- [ ] Obtain an independent review of the complete diff; resolve material findings and verify their regressions.
- [ ] Record commands, exact statuses, numerical verdicts and coverage boundaries.
- [ ] Integrate the qualified changes into dlss-neural-rendering if its state still permits the already authorized integration; do not push or deploy.
