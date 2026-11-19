# Vulkan chain decisions (first safe slice)

```sh
python3 tests/run_all.py --only nr-vk-passes --jobs 1 --strict
```

Compiles `DlssNr_VkChain.h`, the real `DlssNr_VkRecording.h` registry and shared D3D12
`Chain::Routing`/`Schedule` under ASan/UBSan. Checks route/prerequisite refusal, one-pass bypass,
the exact primary creation ticket and submission serial, cancellation/replay/handle reuse, settings failure
latching, overflow-safe budget admission, all ten ping-pong/failure positions and 500 ms settling.
Nine seeded policy defects must compile and fail their own expected assertion; a compiler error or
unrelated crash does not count. Temporary mutant copies never edit the production header.

Source guards keep the runtime/menu one/master while the completion and heap-budget adapters are
missing. This is intentionally **not** a Vulkan multipass runtime test. It runs no Vulkan calls,
NGX, Wine or GPU and says nothing about image quality, synchronization or performance. See the
[design and remaining integration](../../docs/dlssnr/design/nr-vk-passes.md).

## Production submit boundary (step-1 blocker)

`submit_boundary.py` additionally compiles the real host queue forwarding bodies with Vulkan
headers and a fake driver. All 27 combinations of two known queues plus an unknown queue,
success/host-memory failure/device loss, and legacy/two modern callback slots preserve the
arguments and result, forward once, and retain executable recording pins. The driver fake
reenters the real registry to check it is not locked across the call. No callback supplies
creation evidence. The KHR attachment is guarded as source text; actual alias interception,
owned fences and GPU completion are **not** exercised. This characterizes the current blocker;
it must become adapter refusal/success coverage when step 1 is implemented.

The suite now needs the repository's `external/vulkan` headers in addition to GCC. It still
loads no Vulkan driver. For lifecycle coverage/primary-identity witnesses also run
`python3 tests/nr-vk-recording/install_test.py`; those are documented in that suite's README.
