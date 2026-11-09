# Deferred Vulkan owner retirement

`python3 tests/deferred-release/run.py` compiles the production deferred queue, replacing
only its clock and platform/logging includes, under ASan/UBSan. It checks drained-owner
cleanup before device destruction, abandonment of lost/undrained owners, refusal of
late enqueues, numeric-handle reuse, idle-device flushing without retiring the owner, and a collector/retirement race. A barrier pauses
a collector after it detached its callbacks; retirement must wait until those callbacks
finish before the device can die. Source guards hold the NR device hook to that order.

This reproduces the fifth upstream merge's transitive `Shader_Vk` lifetime regression.
It does not execute Vulkan or establish the safety of the upstream age/present heuristic.

The 2026-11-09 integration adds nested callbacks, multi-owner filtering and owner-zero cases.
`device_cases.py` compiles the bundle destroy hook with the real queue: an explicit failed NR drain
cannot become successful merely because a later driver-idle call succeeds; its mutant is rejected.
