# D3D12 renderer: shared state contract

Split step 1 began with declarations only. `DlssNr_Dx12_Internal.h`
contains shared data types and `extern` state in `DlssNr::Dx12Internal`.
Every object remains defined in `DlssNr_Dx12.cpp`, at its original point in the
construction/destruction sequence. Present-route objects use qualified definitions
inside the enclosing `DlssNr` namespace. Later steps move functions while retaining public APIs and bodies.

No singleton, inline variable, allocation, reset, lock or resource-lifetime policy is
introduced. Function-local statics stay in their original functions. Stateful RAII
helpers and `NrEnlarger` remain complete in the owner TU until their own extraction;
header pointer fields only need the latter's forward declaration. Future extraction
must make its complete type/destructor contract available before moving a deletion.
No cross-TU helper API is declared speculatively in this step.

The internal header is not part of the public feature API. It must never include
`pch.h`, define shared storage, or gain a header-local mutex/cache. The global class
implementation uses a CPP-only using-directive. Existing public entrypoints remain
in `DlssNr`. The lock order remains pre-upscale mutex before NR mutex; diagnostics
retain their existing nested publication locks. Since 2026-10-30 the model's NGX
initialisation and feature creation take the driver-init gate innermost, under the NR
mutex (`nr-vulkan-present-host.md`, "The driver-init gate (2026-10-30)"). Shutdown remains in the owner TU.

Source-region host fixtures read moved types from the header through the explicit
`renderer_with_internal_types()` helper; the ordinary source concatenator still
returns only CPP units. Fake-host present glue removes private namespace wrappers and definition qualifications
because its test namespace is artificial. The independent link smoke
checks the shared-state declaration contract across separately compiled consumers.
These fakes do not validate GPU behavior or native Windows D3D12 rules.

Acceptance is the full memory-capped host tier (NumPy enabled), clang-format 22,
Release build, and exact per-frame output hashes plus decision projection against
the immutable pre-refactor DLL's corpus/runs, including warmup. The same-DLL A/A
check alone is not the refactor gate. Present/pre-upscale/bridge routes remain
outside the after-upscale replay's evidence; their bodies are unchanged here.

## Step 7: lifetime/admission unit

`DlssNr_Dx12_Lifetime.cpp` owns the post-contract/status and cadence/pause decisions,
recording admission/tracking, video-memory query, retirement/parking and alternate
surface policy. Shared definitions and their initialization order remain in core.
Bodies, local statics, call sites and release ordering are unchanged. `ParkNrClone`
joins the private internal namespace so present-guide callers share the same
retirement policy. `TickNrRetired`'s default argument lives only in the header;
its body still distinguishes shutdown Ready from ordinary Settled retirement.

Deletion sites include complete scaler/stabilizer/UI-mask/cadence types and the
shared complete `NrEnlarger` with its out-of-line destructor. `nr-enlarge` assembles
retirement from its explicit Lifetime owner while retaining core shutdown/retry.
The function-owner registry guards every moved definition; the state identity
suite continues to pin the original shared storage/initializers. Acceptance adds
all four original-DLL in-flight replay modes to the 19 synchronous variants.

## Step 8: present-guide unit

`DlssNr_Dx12_PresentGuides.cpp` owns present guide cloning, capture, identity,
status and bridge consumption. Private shared helpers and `GuideAsk` belong to
`DlssNr::Dx12Internal`; public feature entry points remain in `DlssNr`. `NowMs`
has one definition in core. Shared storage and initialization order stay in core,
as do present orchestration, shutdown and temporal release. Bodies and local
statics remain unchanged; the baseline body and owner contracts and fake-host
identity/copy fixtures check the extraction without asserting live GPU behavior.
