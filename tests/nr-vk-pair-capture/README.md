# Native Vulkan pair-capture host contracts

Run `python3 tests/nr-vk-pair-capture/run.py`. Artifacts stay in this suite's ignored `artifacts/` directory.

- Production portable rules and registry: serial-only admission, format/domain/size limits, 512 MiB reservation,
  retirement before marker reads, invalidation before all reads, exact executed cookie, discard.
- Production `DlssNr_PairCaptureVk.cpp` compiled against fake Vulkan functions under ASan/UBSan: no idle
  commands/allocations, allocation failure, actual allocation-size budget refusal, two snapshots around the pass,
  serial replay, no early publication, valid black images, unsubmitted recordings, partial cancellation,
  worker snapshots and lost-device abandonment. Fake command recording is separate from execution.
- Six independently compiled mutants must fail assertions: ignore retirement, invalidation, marker, budget,
  allow simultaneous reuse, or restore copies after failed tracking. Compilation errors do not count as kills.
- The existing `tests/nr-capture` suite checks the shared state machine, PNG/PFM pixels, provenance and privacy.

The backend compilation suppresses GCC's missing-field-initializer warning for Vulkan's conventional `sType`
initializers and its pre-existing `FormatUtc` format-truncation warning when inlined with an unrestricted clock.
All other warnings are errors. No GPU, Wine, model, filesystem image writer or game runs in this suite.
GPU validation must still prove the shader's actual pixel values, image-view compatibility, barriers,
recording-hook coverage, replay and real driver loss. See the
[design and qualification plan](../../docs/dlssnr/design/nr-vk-pair-capture.md).
