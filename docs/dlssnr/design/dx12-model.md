# D3D12 renderer extraction: model unit (step 2)

`DlssNr_Dx12_Model.cpp` owns model result names, the proxy probe, direct/forwarder
selection, capability parameters, fault reporting, scaling/float-slot diagnostics,
extras/tuning snapshots, and the public D3D11 probe. Whole functions and their
function-local statics move verbatim from the renderer. Shared storage stays in
`DlssNr_Dx12.cpp`; its initialization order and the internal namespace are unchanged.
Only helpers called by the remaining renderer gain internal-header declarations.
The D3D11 probe keeps its public namespace/signature and its existing lock scope.

There is no model/backend, parameter, fault, locking, lifetime or default-policy
change. The new compile unit is registered in the fork-owned project/filter tails.
Inputs, resource helpers, lifetime/admission, Dispatch and routes remain in place.

Acceptance: full memory-capped host tier with NumPy; clang-format 22; Release under
the shared prefix lock; all 17 extended variants compared bit for bit and decision
for decision against the immutable b2eed6e3 DLL, including direct loading and warmup.
The replay judge is disabled for this identity gate; this is not a new quality
calibration. Source checks pin the moved function definitions to the pre-extraction
bodies, including statics. Source-region fixtures follow the new TU boundaries.

`ProbeD3D11` is outside replay coverage. Host cases exercise its actual body against
recording loader/COM fakes (null/off, direct guard, one-shot behavior, adapter choice
and cleanup). Release checks real MSVC/D3D interfaces and linking. Neither proves a
live D3D11 game's/model's response: that runtime evidence remains uncollected here.
