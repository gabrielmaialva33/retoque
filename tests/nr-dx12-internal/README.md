# Internal D3D12 renderer state linkage

`python3 tests/nr-dx12-internal/run.py` compiles the verbatim internal header with
platform/domain include stubs, actual owner definitions, and two independent
consumer translation units. It compares all 96 addresses with the owner's,
mutates representative model/pre/present/cadence state across both consumers,
and checks that missing and duplicate owner definitions fail at link time.
The fixed contract pins definition order and initializer tokens to `ffb4eea6`.

This is a storage/linkage test, not a D3D12 or domain-logic simulation. The real
Windows includes and types are checked by the Release DLL build. Existing source
slice suites exercise domain logic; frozen-baseline replay checks actual outputs
and decisions on the supported after-upscale route. Do not describe this test as
GPU lifetime, performance, or native Windows runtime validation.

`function-owners.json` pins extracted definitions to one renderer unit each;
adding a declaration elsewhere does not satisfy a definition signature.
`resource-contract.json` pins the eleven resource helper definitions to the
step-4 base (`3a05c194`), ignoring formatting whitespace only. The scratch
generation and descriptor-reuse helper must remain in the core. Resource RAII
behavior is executed by `nr-before-upscale` against its strict host fakes;
its declarations are also compiled here through the real internal header.
