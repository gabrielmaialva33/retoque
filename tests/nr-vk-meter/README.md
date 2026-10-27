# Vulkan meter allocation ownership

Run from the repository root:

```sh
python3 tests/nr-vk-meter/run.py
```

The runner uses `CXX` or `g++`. Select additional installed compilers with repeated
`--compiler` arguments. It compiles the exact production
`MeterReadbackReady`, `CreateMeterReadback`, and `DestroyMeterReadback` bodies with
ASan, UBSan and warnings treated as errors. `--source` accepts a canonical staged
source for development; ordinary runs read the production feature file.

This is a host-only allocation test. A narrow typed fake Vulkan API tracks buffer,
memory, bind and map ownership. No Vulkan loader, installed SDK headers, driver,
model, or GPU is required. Fake API types reproduce the used source boundary;
this suite does not establish SDK ABI compatibility.

## Contract

Every slot is empty or has a complete buffer/memory/mapped-pointer tuple. Failed
construction releases its local resources without publishing partial ownership.
Earlier complete slots survive, and retry fills the remaining slots. A complete
ring does not allocate again. Readiness rejects all seven incomplete tuple
combinations.

The suite injects buffer-creation, memory-type, allocation, binding and mapping
failure in each of four slots. It checks handle ownership before and after retry,
exact resource identity for completed predecessors, full and mixed-state cleanup,
slot clearing, frame-counter reset, and repeated destruction.

Cleanup follows the project's conservative ownership contract: unmap, destroy the
buffer, then free its memory. The fixture's rejection of a different order is a
project-contract check; it does not claim that freeing memory while a bound,
unused Vulkan buffer still exists is inherently invalid Vulkan usage.

## Limits

The helper's complete-tuple predicate is executed directly. Wiring of the
feature's GPU-copy guard needs feature integration coverage. The allocation suite
does not establish command-buffer retirement, GPU completion, device-loss safety,
concurrent ownership, or the safety of reading a sample after an elapsed frame
count. Those require the recording-retirement helper and feature integration.

The default enables LeakSanitizer. Environments that block LeakSanitizer's process
inspection can use `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`; ASan and UBSan
remain active, and the fake-object ledger independently checks every Vulkan object
for orphaned ownership and duplicate release.
