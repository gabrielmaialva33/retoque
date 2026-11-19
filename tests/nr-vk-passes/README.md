# Vulkan chain decisions (first safe slice)

```sh
python3 tests/run_all.py --only nr-vk-passes --jobs 1 --strict
```

Compiles `DlssNr_VkChain.h`, the real `DlssNr_VkRecording.h` registry and shared D3D12
`Chain::Routing`/`Schedule` under ASan/UBSan. Checks route/prerequisite refusal, one-pass bypass,
the exact creation ticket and submission serial, cancellation/replay/handle reuse, settings failure
latching, overflow-safe budget admission, all ten ping-pong/failure positions and 500 ms settling.
Eight seeded policy defects must compile and fail their own expected assertion; a compiler error or
unrelated crash does not count. Temporary mutant copies never edit the production header.

Source guards keep the runtime/menu one/master while the completion and heap-budget adapters are
missing. This is intentionally **not** a Vulkan multipass runtime test. It runs no Vulkan calls,
NGX, Wine or GPU and says nothing about image quality, synchronization or performance. See the
[design and remaining integration](../../docs/dlssnr/design/nr-vk-passes.md).
