# Vulkan meter recording retirement

```sh
python3 tests/nr-vk-meter-readback/run.py
```

Uses `CXX` or `g++`, with ASan, UBSan and warnings treated as errors. Repeat
`--compiler` to exercise additional installed compilers. For a staged draft,
`--recording-root` supplies a root containing the existing production
`retoque/dlssnr/DlssNr_VkRecording.h`.

The tests use the actual `VkRecording::Registry` to retain a meter reservation pin
through recording and `End`. A completed/submitted/resubmitted executable command
buffer remains pin-owning until the registry observes retirement, so repeatedly
checking frame age cannot make the sample readable or reusable. The test drives
actual registry Begin/End/pin retirement; its completion and resubmission scenario
is modeled by retaining that same executable recording, without issuing Vulkan.

Other cases cover all four ready-mask bits, zero and out-of-range masks, saturation,
retired-but-unconsumed occupancy, cancellation identity, stale cancellation after
reuse and abandonment, invalidation without freeing a slot, surviving tokens after
ring destruction, out-of-order serials, serial exhaustion, allocation failure before
publication, and a cross-thread release/acquire handoff read before joining the
producer thread.

Only the serialized ring owner may call its methods. The last pin can be released
on another thread. Retirement tokens own independent shared state; no token owns a
ring pointer, mapped memory, device, or Vulkan resource. Ring retirement does not
itself prove GPU completion. Actual lifecycle callbacks must retain their existing
GPU-safety preconditions. No elapsed frame count is used as evidence of retirement.

`red-placeholder.log` is deliberately labeled **new-helper contract development**:
it tests a temporary immediate-age placeholder, not canonical production code.
Feature integration tests are responsible for proving that the production route
uses this helper and clears float zero to `0.0f` before recording the copy. This
suite neither changes nor emulates the feature's 256-byte buffer or GPU commands.
