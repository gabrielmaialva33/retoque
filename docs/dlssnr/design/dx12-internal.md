# D3D12 renderer: shared state contract (split step 1)

The Phase 5 extraction starts with declarations only. `DlssNr_Dx12_Internal.h`
contains shared data types and `extern` state in `DlssNr::Dx12Internal`.
Every object remains defined in `DlssNr_Dx12.cpp`, at its original point in the
construction/destruction sequence. Present-route objects use qualified definitions
inside the enclosing `DlssNr` namespace. Public APIs and function bodies do not move.

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
retain their existing nested publication locks. Shutdown remains in the owner TU.

Source-region host fixtures read moved types from the header through the explicit
`renderer_with_internal_types()` helper; the ordinary source concatenator still
returns only CPP units. Fake-host present glue drops only the namespace qualification
of definitions because its test namespace is artificial. The independent link smoke
checks the shared-state declaration contract across separately compiled consumers.
These fakes do not validate GPU behavior or native Windows D3D12 rules.

Acceptance is the full memory-capped host tier (NumPy enabled), clang-format 22,
Release build, and exact per-frame output hashes plus decision projection against
the immutable pre-refactor DLL's corpus/runs, including warmup. The same-DLL A/A
check alone is not the refactor gate. Present/pre-upscale/bridge routes remain
outside the after-upscale replay's evidence; their bodies are unchanged here.
