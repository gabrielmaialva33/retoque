# Vulkan pair capture implementation plan

> Execute inline; the user forbids additional agents and commits. Design approval is already given.

**Goal:** tracked-only same-execution capture on native Vulkan after-upscale NR.
**Architecture:** shared split PairCapture session; recording-retirement backend; capture-only sampled
float32 shader; common CPU writer with Vulkan ownership serialized by the existing route mutex.
**Tech stack:** C++20, Vulkan, HLSL/SPIR-V, Python host runners.
**Spec:** [approved design](../../dlssnr/design/nr-vk-pair-capture.md).

## Constraints and review focus

Only this worktree. No config/default change; no idle allocations or GPU work; 512 MiB total bound;
no completion by frame count; device invalidation precedes registry forgetting; one final Release build.
Review partial recording exceptions, executable replay, queued writer versus device teardown, unsupported
view/domain refusal and descriptor/buffer reuse. These each need host behavioral tests and mutants.

## Tasks

1. Extend tests/nr-capture/test.cpp for split begin/original/commit, abort and discarded completion;
   observe failure, implement in DlssNr_PairCapture.h, retain lead's tracked-only and D3D12 semantics.
2. Add tests/nr-vk-pair-capture against a portable production rules header: format/size budgets, retirement
   token and marker, abandonment, stale tickets, replay/refusal and partial state. Observe RED, implement,
   run GREEN and independently compile/run safety mutants which must fail assertions.
3. Implement Vulkan snapshot resources/pass and own HLSL; generate SPIR-V/header using an isolated shader
   compiler path, without touching the shared Wine prefix during another build. Integrate two boundaries,
   polling and shutdown in DlssNrFeature_Vk.cpp under g_vkMutex; add production-body fake coverage.
4. Share writer output through CPU-row jobs; route request/status only to a capable backend; handle worker
   lifetime, bounded CPU staging, atomic invalidation and teardown. Preserve D3D12 output tests and UI states.
5. Register suite, update design/module documentation and catalog; clang-format 22 changed C++ lines;
   run focused suites/mutants/docs. Self-review ownership and off path. Run exactly one final
   ./build-local.sh Release > ../retoque-vk-pair-capture-build.log 2>&1; report outcome and limits.

The work-state list records progress and any necessary rulings. No full host tier or runtime qualification.

Implementation and targeted host checks are complete; the adjacent delivery report records the final build and limits.
