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

The OS constructor is the boundary substitute: it supplies the original pipeline,
sampler, three descriptor sets and mapped buffer. The NR constructor executes
unchanged, including checked buffer allocation and replacement of the unused base
pipeline with the matching NR-only RGBA16F module. Tests reject incompatible output
formats without effects, preserve failure ownership, prevent the old pipeline from
becoming a fallback, and compare selected shader bytes for all 16 filter/direction
combinations.

`python3 tests/nr-vk-scaler/run.py` first checks the actual embedded SPIR-V for all
nine variants: RGBA16F storage, set 0/binding 2, four-component writes, `CSMain`,
original workgroup sizes, and byte identity with `precompile/scaler/*.spv`.
`shader_contract.py --upstream-root .` applies that contract to the inherited
upstream arrays as a regression control; it is expected to fail for those arrays.

Regenerate only the fork-owned modules with
`python3 retoque/shaders/dlssnr/precompile/build_scaler_vk.py`. This requires native
DXC with SPIR-V support and SPIRV-Tools; `--dxc` and `--spirv-val` select executables.
The generator validates every module and the embedded header before publication,
and records tool identities and source/artifact hashes in its adjacent JSON manifest.
Upstream shader sources and compiled artifacts are not rewritten. Real GPU
execution and image-quality qualification remain separate from these host checks.
