# NR Vulkan scaler recording regression

`python3 tests/nr-vk-scaler/integration.py --root .` builds extracted, unmodified
production constructor/Reserve/Dispatch bodies with real Vulkan SDK types and a
controlled fake GPU boundary. GCC and Clang run with ASan/UBSan by default;
`--compiler g++` selects one compiler. Set `VULKAN_HEADERS` if necessary.

Against an earlier checkout without `DlssNr_Scaler_Vk.cpp`, the fixture executes
actual `OS_Vk::Dispatch`. A compatibility adapter supplies no-op reservation only
so the same tests can expose old behavior instead of failing to compile.

The fixture holds descriptor references until simulated execution and executable
resubmission. This detects shared constant writes and live descriptor reuse. It
also checks three-slot admission, retained leases after End and submission,
release after reset, stale/wrong-device tickets, wrong owner/command buffer,
moved batches, invalid images/dimensions, and FSR payload bytes/range/alignment.
Rejected operations must leave both the Vulkan/timing effect counter and every
mapped byte unchanged.

The OS constructor is the boundary substitute: it supplies the pipeline, sampler,
three descriptor sets and original mapped buffer. The new scaler constructor is
executed unchanged, including replacement allocation and mapping. Shader binary
compilation and real driver/GPU execution remain separate smoke-test concerns.
