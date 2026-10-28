# Vulkan resource completion and sustained FG Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Tasks use independent drafts; root alone writes canonical remote files.

**Goal:** Close the approved Vulkan resource gaps and qualify sustained provider counters with reproducible evidence.
**Architecture:** Fork-owned scaler leases and transactional readback allocation preserve the existing recording/generation registry. Duration-based provider tests and a serial report entry reuse current fixtures.
**Tech Stack:** C++20/MSVC, Vulkan, Python3, Bash, GCC/Clang sanitizer host fixtures, Wine/Proton.
**Spec:** docs/dlssnr/design/vulkan-readback-scaler-qualification.md

## Global Constraints
- Preserve non-NR upstream paths, NR configuration defaults, upstream shader source/bytecode and Ada unlock opt-in. The direct 1.25 GPU regression requires NR-only RGBA16F shader variants with unchanged filter arithmetic; see the design's qualification finding.
- Vulkan recordings retain resources until successful reset/rebegin/free; completion alone is insufficient for reusable command buffers.
- Destroy only at a proven safe boundary; device abandonment performs no GPU callbacks.
- clang-format22; host tests do not prove GPU behavior; exact binary hashes identify runtime evidence.
- No model download, game deployment, display change or remote push.
- Root is the only Winx operator. Independent drafts/tests are local; canonical writes use fresh ReadFiles revisions and EditFiles.

## Review Focus
- Partial construction followed by retry: no slot may be admitted from a non-null buffer alone.
- An executable recording resubmitted after a newer frame: descriptor and constant bytes remain original.
- Discarded and delayed meter recordings: no unexecuted/stale sample is consumed by frame age.
- Duration metadata claiming elapsed time/windows unsupported by SAMPLE chronology: qualification refuses.
- Failed/unavailable GPU scenario or different DLL hash: consolidated report cannot claim complete coverage.

## Task 1: Transactional exposure readback
Files: retoque/dlssnr/DlssNrFeature_Vk.cpp; tests/nr-vk-meter/{run.py,cases.cpp,README.md}; tests/suites.toml.
Interface: MeterReadbackReady(slot), CreateMeterReadback(), DestroyMeterReadback(); keep existing state arrays.
- [x] Reproduce baseline partial-allocation failure using actual production bodies (GCC and Clang).
- [x] Publish only fully built local tuples; clean partial locals; gate copy admission on readiness.
- [x] Run failure/retry/destruction fixture with GCC and Clang sanitizers and review.

## Task 2: NR-specific recording-safe scaler
Files: retoque/shaders/dlssnr/DlssNr_Scaler_Vk.{h,cpp}; retoque/dlssnr/DlssNrFeature_Vk.cpp; OptiScaler/OptiScaler.vcxproj and filters; tests/nr-vk-scaler; tests/suites.toml.
Interface: optional<Batch> Reserve(VkCommandBuffer,uint32_t), move-only Batch, bool Dispatch(VkCommandBuffer,Batch&,const VkImageInfo&,const VkImageInfo&).
- [x] Demonstrate old shared constants/ring overwrite with behavioral controls.
- [x] Implement bounded3slot immutable reservations using VkRecording; validate before any effect.
- [x] Reserve each needed scaler before the NR frame's first command and consume its Batch at dispatch.
- [x] Verify state/constant/descriptor bytes, saturation, stale/foreign batches and valid reset reuse; review non-NR isolation.
- [x] Correct the observed BCUS three-component store and source-proven scaler format mismatch with NR-only RGBA16F variants; verify actual SPIR-V contracts and pipeline failure paths.
- [x] Retest the isolated WorkingScale=1.25 route with the repaired DLL and preserve the original failing evidence; invocation `6825c786d5e1430895c04c7fa2c8254a` passed on 2026-10-28.

## Task 3: Readback execution and retirement
Files: fork-owned readback metadata helper if needed; DlssNrFeature_Vk.cpp; nr-vk-meter behavioral fixture.
Interface: per-slot recording token with release/acquire retirement publication, zero sample sentinel, and monotonic serial; consume only after legal recording retirement.
- [x] Review the execution/retirement proof against Vulkan contracts and reproduce frame-count admission under delayed/discarded work.
- [x] Implement bounded reservation, zero sentinel and retirement consumption, with last-valid fallback; invalidate before abandonment forgets pins.
- [x] Verify resubmission, saturation, discard, owner retirement, generation replacement and abandon without GPU callbacks.

## Task 4: Sustained provider qualification
Files: tests/fg-provider-d3d12/{run.py,harness.cpp,duration_schedule.h,test_qualification.py,README.md}.
Interface: --steady-seconds integer5..120, absent0; assess(text,multiplier,steady_seconds=0); REQUEST and DURATION protocol checked against samples.
- [x] RED tests for duration policy and contradictory/missing evidence.
- [x] Preserve quick96/48/96/96 with32warmup; extend active phases by duration and full5s windows; retain existing pacing.
- [x] Derive deadline240+3*(seconds+5) in long mode; preserve owned cleanup.
- [x] Run pure host tests and actual C++ schedule helper; review.

## Task 5: Consolidated validation and documentation
Files: tools/test-rendering.sh and focused reporting helper; .github/workflows/nr-tests.yml; tools/README.md; tests/README.md; docs/dlssnr/roadmap.md; verification record.
- [x] Add strict required-host execution, successful/failed CI evidence retention, serial Vulkan modes and retained invocation receipts.
- [x] Correct only verified roadmap staleness (Stage1 measurement and existing Windows installer).
- [x] Run full host suite, pinned format check, Release and available GPU fixtures serially with final DLL identity; required invocation `ee577bc1e13b48a8aecc7cd0ab84f46b` passed, including all 94 host suites.
- [x] Independent review, preserve raw findings and qualification limits, commit separate reviewed units.

## Review resolutions

- Check `vkBindBufferMemory` in the NR scaler's local transaction; the shared helper ignores its result and remains untouched for upstream users.
- Do not free values written to failed allocation output parameters; ownership begins only after success. Poisoned-output controls cover the boundary.
- Use atomic release/acquire publication from the final recording token; weak ownership counts alone do not publish mapped writes across threads.
- The consolidated entry rejects empty/malformed moving measurements, terminalizes startup hash failures, preserves timeout/abort when receipt writing fails, and stops on every non-PASS step.
- The loopback interrupt guard and shared cleanup helper close a detached-child escape on outer timeout. The inner grace is two seconds, beneath the unchanged outer four seconds; no global Wine cleanup is introduced.
- Independent Vulkan and orchestration reviews are clear. The canonical strict host run executed all 94 suites without skips, including native process identity checks.
- The four unchanged Vulkan scenarios do not execute exposure sampling or WorkingScale above 1. The separate 1.25 probe now covers source-backed up-leg execution; exposure sampling, independent down-leg execution, GPU resubmission and image-quality qualification remain explicit gaps.

Final evidence and preserved attempts: [qualification record](../reviews/2026-10-27-vulkan-readback-scaler-qualification.md).
