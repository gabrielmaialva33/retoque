# Internal D3D12 renderer state linkage

`python3 tests/nr-dx12-internal/run.py` compiles the verbatim internal header with
platform/domain include stubs, actual owner definitions, and two independent
consumer translation units. It compares all 96 baseline addresses plus the explicitly registered `g_nrDeviceOwner` extension with the owner's,
mutates representative model/pre/present/cadence state across both consumers,
and checks that missing and duplicate owner definitions fail at link time.
The fixed contract keeps the original 96 names, relative declaration/definition order and
initializer hash pinned to `ffb4eea6`. Extensions are separate metadata: the new owner is
declared before the baseline extern block but defined after coverage state and before
`g_nr`. Declaration order is not assumed to equal definition order for extensions.
Its exact declaration/definition and placement are checked independently, without
regenerating or changing the original baseline hash.

Both independent consumer TUs admit devices through the same production owner object;
the fixture checks one retained AddRef, same-device repeat and second-device refusal.
The missing/duplicate-owner negative controls also require diagnostics for the new owner.
Platform `AddRef`/`Release` is a minimal reference counter for this linkage check, not a COM
or D3D12 simulation. The production owner stays retained until process exit; this fixture
resets it before its stack device disappears.

This is a storage/linkage test, not a D3D12 or domain-logic simulation. The real
Windows includes and types are checked by the Release DLL build. Existing source
slice suites exercise domain logic; frozen-baseline replay checks actual outputs
and decisions on the supported after-upscale route. Do not describe this test as
GPU lifetime, performance, or native Windows runtime validation.

`function-owners.json` pins extracted definitions to one renderer unit each;
adding a declaration elsewhere does not satisfy a definition signature.
`resource-contract.json` preserves the eleven original helper entries from the step-4
base (`3a05c194`), ignoring formatting whitespace only. The two intentional hardening
updates (`CreateGuideClone`, `ReadableGuide`) are individually recorded; the other nine
hashes stay unchanged. Four new helpers (`DeviceChildBelongsTo`, `GuideCopyFamily`,
`GuideCopyCompatible`, `PresentGuideCopyCompatible`) have explicit extension hashes and
single-definition ownership entries. The scratch
generation and descriptor-reuse helper must remain in the core. Resource RAII
behavior is executed by `nr-before-upscale` against its strict host fakes;
its declarations are also compiled here through the real internal header.

The 2026-10-23 ReadableGuide correction intentionally updates its resource-body hash only;
`nr-guide-copy-contract` exercises the new copy-shape/retirement behavior. Other extraction hashes stay pinned.

The 2026-10-27 changes are explicit device/copy hardening, not a new extraction baseline.
Only the two intentionally changed resource-body hashes are updated. The four new helper
hashes freeze the added contract; behavioral RED/GREEN is in `nr-guide-copy-contract`.
No production declaration/definition was reordered to satisfy this fixture.
