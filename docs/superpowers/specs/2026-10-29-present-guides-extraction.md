# D3D12 present-guide extraction

Status: first bounded extraction accepted in the conversation; execution may be delegated through Herdr.
Base: `dd2a589108652870a702313678cf67cf8a98ced7`.

## Intent and approach

Move the present-guide capture/identity/consumption responsibility out of the remaining D3D12 renderer without changing pixels, decisions or resource lifetime. Keep upstream paths and the existing responsibility-unit architecture. A wholesale renderer rewrite and mixing this move with ViT reuse, LSFG adaptation or defaults are explicitly excluded. The incremental approach is the one proposed and accepted in the conversation.

## Contract

- New unit: `retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp`. Follow the existing PCH and private `DlssNr::Dx12Internal` conventions.
- Candidate scope is the present-guide helpers currently surrounding `CreatePresentGuideClone`, `EvaluateOutput`, `D3D12OutputExtent`, `StampPresentGuide`, guide identity/reporting, `CaptureGuides`, `CaptureTemporal`, `CaptureBridgedGuides`, and the later `ReadCapturedGuides` / `CapturedPresentGuides` / `AbandonCapturedGuides` block. Enumerate exact moved functions against the base before editing. Keep an unrelated shared helper in core if extracting it would broaden this scope.
- Keep every existing shared storage definition, initialization order and original initializer in its owning core translation unit. In particular do not move the ring, clone array, bridge-read state, guide identity/status atomics, route state or mutexes. Preserve the pinned state contract.
- Preserve function bodies, local statics, public API signatures, call order, lock order, RAII destruction, copy/resource flags, recording and completion tracking, resets, fallback/lag behavior, logging and default-off paths. Only necessary linkage/namespace spelling and declarations may change.
- `RunPresentPass`, `Dispatch`, `EvaluateAfterUpscale`, synthesized-motion implementation and core shutdown remain in core. A moved guide helper called by them retains its semantics; no general lifecycle cleanup is authorized.
- Register the new translation unit in the fork-owned project/filter tails. Adapt existing source-region fixtures to explicit owners rather than removing assertions or retaining duplicate production copies.
- Do not modify INI, HLSL/bytecode, model/FG features, GPU settings, VRR, game installations or unrelated work. No commit, merge, push or publication in this task without a later explicit authorization.

## Validation and evidence

Freeze the exact source base and keep any available pre-extraction DLL/forwarder/corpus by hash; an existing build must not be relabeled as compiled from the current commit. Original evidence is never overwritten.
Run existing guide/state host tests before and after; add extraction ownership/body-preservation and missing/duplicate-definition negative controls before moving production code, then observe the expected RED/GREEN transition. Compile real moved functions through existing fakes, not rewritten logic.
Run the full host tier with at most four workers, clang-format 22, documentation/catalog and diff checks. A Release build and relevant MSVC/route checks are separate gates, serialized through the existing compiler-prefix lock. Do not kill/reset a shared Wine prefix or launch games.
After-upscale frozen-binary replay can establish only that route's equality; it does not certify present or D3D11. New live-route coverage must be identified separately. Missing GPU/native-Windows evidence stays pending, never silently waived to claim runtime equivalence.

## Delegation boundary

One Codex implementer owns the extraction and necessary test/project changes in an isolated worktree. The coordinating assistant preserves baseline, runs independent checks and controls any build/GPU admission. Claude reviews boundaries and then the final diff read-only. Neither delegate may create further agents or edit the main checkout.
