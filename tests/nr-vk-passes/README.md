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

Source guards keep the runtime/menu one/master while generation integration is
missing. This is intentionally **not** a Vulkan multipass runtime test. It runs no Vulkan calls,
NGX, Wine or GPU and says nothing about image quality, synchronization or performance. See the
[design and remaining integration](../../docs/dlssnr/design/nr-vk-passes.md).

## Historical production submit boundary (before step 1)

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

## Owned completion adapter (step 1c)

The production submit fixture now exercises 54 inactive combinations, plus registered devices
with Passes=1 and active submit/submit2/shared-KHR callbacks, including nested dispatch. Inactive
forwarding preserves arguments/counts and adds zero observer locks or Vulkan calls. The real
registration helper refuses absent certificates, protected queues and multiple physical devices.
`creation_cases.cpp` executes the real bounded observer under sanitizers with a counted driver:
exact epochs/serials, primary ONE_TIME_SUBMIT admission, reverse completion on two queues, pending
and completed replay, failed app/marker submits, reset/free/pool/reuse, lost-device polling and
abandonment, off intervals and ten-slot exhaustion. Unknown marker completion is retained until
a proven drain. These are host proofs of the adapter, not GPU/Detours or renderer-chain execution.
The prior characterization paragraphs describe the initial boundary before steps 1a-1c.

## Step-2 attribution boundary

`budget_boundary.py` executes production `FindMemoryTypeIndexOn` against two synthetic legal
device-local heaps. Different requirement masks select different heaps. The same scalar model
estimate admits on one heap and refuses on the other; summing the heaps hides that refusal.
This is a counterexample to aggregate-budget admission, **not** a trace of actual NGX allocations.
The model stats callback supplies bytes only, not memory types or heap identity. The initial attribution review was resolved by the lead's unique-local-heap restriction.
`budget_cases.cpp` executes the live-query adapter with synthetic properties2 results, checking
unsupported extension, several/no local heaps, unknown/mixed types, malformed/unwritten/stale
queries, overflow, device reuse/loss, effective-settings refusal latching and two bounded retained
slots including permanent abandonment. The real host admission body is compiled in the submit
fixture; Passes=1 forbids heap allocation and proves no query, log or observer lock. This does not
exercise a driver budget or allocate a chain generation.

## Step-3 queue boundary

The creation fixture also proves that A can be Ready on one queue while a later unwatched
primary ONE_TIME_SUBMIT recording B is forwarded on another queue, without a completion marker
for B. A's creation evidence therefore does not establish B's ordering or queue-family ownership.
This is a boundary witness, not a failure of the creation observer or an executed GPU hazard.
