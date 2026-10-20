# D3D12 renderer extraction: resources (step 4)

Move CreateScratch/CreatePreUpscaleScratch, IsSrgb, Barrier, TypedGuideFormat,
IsTypeless, CreateGuideClone, ExposureTextureUsable, ReadableGuide,
FormatCanHoldLinearHdr and GetResource to DlssNr_Dx12_Resources.cpp. Resource-state
RAII declarations (RestoreResourceState and ReadResourceScope) go in Internal.h;
their method bodies move out of line to Resources.cpp. Preserve member order,
aggregate initialization, constructor initialization, destructor behavior and all
function bodies. ScopedNrStateEnvelope stays with Dispatch: it is hook policy,
not a resource-state scope.

The scratch generation and DescriptorReuseAvailable remain in the core, with the
single storage definition and original initialization order. ReadableGuide calls
the existing ParkNrResource through an internal declaration; lifetime/admission
policy stays in the core. No caller, resource flag, state, barrier ordering,
format choice, descriptor policy, shader or default-off behavior changes.

Register the new unit in the project/filter fork tails. Replace cross-file source
slice boundaries with explicit definitions; host pre-upscale fixtures assemble the
actual shared RAII declarations and out-of-line methods. Pin unchanged helper
bodies to the base revision with whitespace-insensitive hashes (clang-format may
realign relocated signatures). Preserve descriptor-generation/linkage checks.

Gates: memory-capped host tier with NumPy, clang-format 22, diff check, Release
under the shared prefix lock, MSVC present-guide identity suite under that lock,
and all 17 frozen replay variants with calibration/judge disabled. Replay checks
pixels and decisions including warmup against b2eed6e3, not a new quality metric.

Pre-upscale/present routes are not executed by the after-upscale replay corpus.
nr-before-upscale executes actual scratch allocation, RAII and scoped boundary
logic against fakes under ASan/UBSan; nr-dispatch exercises actual descriptor
reuse/generation behavior. The MSVC identity suite checks present-guide identity
glue using Windows compiler/library with fakes, not real D3D12. Release validates
Windows headers/linking. None establishes live pre-upscale/present behavior or
native Windows debug-layer correctness; those remain explicit coverage limits.
