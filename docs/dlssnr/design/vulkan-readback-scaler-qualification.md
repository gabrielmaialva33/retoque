# Vulkan readback, scaler lifetime and sustained FG qualification

Status: implemented on qualified production source `d845b511`; the required serial run and separate WorkingScale=1.25 regression passed. Scope was approved on 2026-10-27, isolated from dlss-neural-rendering at ff7dc45b.
Scope: the next code batch recommended and accepted with "Vamos la entao." Performance experiments, replay format transitions, native-Windows qualification and game deployment remain separate work.

## Intent and boundaries
Complete the remaining NR Vulkan scaler descriptor/constant lifetime boundary, make exposure readback allocation transactional, remove the readback's reliance on elapsed recordings, and make the existing provider fixture capable of sustained counter qualification. Reuse the existing tests and process ownership. Preserve NR settings, filter arithmetic, upstream shader artifacts, Stage defaults, Ada unlock defaults and the user's display settings. The direct supersampling regression below requires correcting the NR-only storage-image contract before integration.

## Approach
Keep upstream OS_Vk behavior intact. A fork-owned DlssNr_Scaler_Vk uses a bounded three-slot pool per scaler instance; each descriptor and aligned constant slice is immutable for the complete command-buffer recording lifetime. Full-frame reservations precede GPU recording, including timing and encode. The existing generation pin protects the scaler objects themselves. Its selected NR-only shader must declare the actual RGBA16F storage target and write all four components; the chosen RGB filter algorithm is preserved.

Build each exposure readback slot in local handles; publish buffer, bound memory and mapping only as a complete tuple. On failure, unwind only newly allocated resources. Previously completed slots survive and retries are safe. The copy path requires complete readiness.

Readback completion is independent of allocation. A recording may be discarded or resubmitted. A CPU read needs legal retirement of that recording, so it cannot execute again, plus a valid GPU-written sample. Independent review selected a bounded four-slot ring and a separate CPU retirement state: the pinned token publishes with a release store, and consumption observes it with an acquire load. Initialize float zero to 0.0 only while the slot is exclusively CPU-owned; a discarded recording then fails the existing finite-positive exposure predicate. Keep the existing 256-byte copy and transfer-to-host barrier. Never consume on frame-count age alone or wait idle per sample. Saturation retains the last valid exposure. Setting changes invalidate pending results without releasing live slots; abandonment clears metadata and mapped handles before forgetting device pins. An older serial cannot replace a newer accepted sample. Tokens own no GPU callbacks or raw route pointers.

Extend the provider fixture with explicit --steady-seconds 5..120 (absent means current quick mode). Keep 32 warmup frames, at least the original frame count per phase, existing pacing, disable/reenable/resize controls, and the exact five-second ratio/capability rules. Long active phases require requested steady duration and floor(seconds/5) complete windows. Monotonic sample chronology is authoritative; request and phase records must agree. Runtime deadline is bounded and derived from the requested duration. x3/x4 and Ada unlock remain explicit.

Extend the serial rendering entry and CI evidence handling rather than create a parallel test system. A consolidated receipt identifies the DLL hashes, invocation, executed scenarios and evidence limits. Include the four existing NR Vulkan modes, preserve raw validation findings, and retain successful as well as failed CI reports.

The serial entry keeps its existing fail-fast behavior. Reuse `tests/run_all.py` supervision; the loopback's detached child cleanup must finish its two-second grace inside the outer four-second grace. A secondary receipt failure preserves TIMEOUT/ABORTED and the actual exit code. Initial hash failures produce terminal reports. Moving receipts validate concrete samples and hashes, not collection lengths alone.

Coverage is explicit: the four existing Vulkan scenarios do not provide an exposure image or select WorkingScale above 1. They cover runtime integration and allocation setup, while the new production-body host fixtures establish meter/scaler ownership and failure behavior. A separate isolated WorkingScale=1.25 route probe exercises the up-leg, with its own invocation and binary identities; it does not independently prove down-leg execution, exposure sampling, executable-recording resubmission or image quality.

### Supersampling regression found during qualification

The first direct 1.25 probe produced ten `VUID-vkCmdDispatch-OpImageWrite-08795` messages: inherited BCUS wrote a three-component texel into RGBA16F. Inspection also found that the inherited four-component downscalers and FSR declared `Rgba32f` while NR allocates RGBA16F targets. This second format mismatch is a source/SPIR-V finding, not an additional observed validation message. Both contracts require an NR-only correction before this batch can integrate.

Compile the unchanged downscaler/FSR algorithms through small fork-owned wrappers that declare `rgba16f`. The fork-owned BCUS variant retains its RGB arithmetic and writes `float4(Result, 1.0)`, matching the other filters' opaque intermediate alpha; the NR resolve restores the original frame alpha. Preserve every upstream shader source and compiled artifact. Test the actual generated SPIR-V image format and texel width, header/binary identity, pipeline selection/failure behavior, then rerun the direct GPU probe against the repaired DLL. Do not erase or relabel the failing receipt.

Contract references: [DXC SPIR-V image formats](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/SPIR-V.rst) and [Khronos storage-image compatibility](https://docs.vulkan.org/guide/latest/storage_image_and_texel_buffers.html).

Qualification completed on 2026-10-28: the repaired 1.25 route executed all three 1600x900 builds and 70 active evaluations with zero host validation errors and no recurrence of `08795`. Six known native-model `09600` messages remain separately attributed. The [qualification record](../../superpowers/reviews/2026-10-27-vulkan-readback-scaler-qualification.md) preserves the original failure, later startup timeouts, environment recovery, exact artifact hashes and remaining coverage limits.

## Alternatives
A larger global Shader_Vk rewrite was rejected for this batch because it affects unrelated upstream users. Increasing ring size or assuming a fixed frame lag does not establish ownership or completion. Broad new game defaults are deferred until image-quality and cost comparisons exist.

## Acceptance
- Fault injection at every readback construction stage and slot proves no partial publication, leaks, invalid copy admission or failed retry.
- Scaler tests execute production bodies and verify actual descriptor references and constant bytes, saturation before effects, reset/rebegin/free reuse, ended recordings and stale batches.
- Readback tests delay/discard/resubmit recordings; only retired GPU-written samples can be consumed, with bounded memory and no abandoned-device GPU calls.
- The quick provider schedule and its result semantics remain compatible. Long mode rejects invalid duration, missing/conflicting records and claimed windows not supported by samples.
- Full host aggregate, Release build, available Vulkan and x2 provider runtime execute against recorded final DLL hashes. Report any unavailable platform/scenario or unresolved validation message.
- Format C++ with clang-format22. No model download, game deployment, display change or remote push.
