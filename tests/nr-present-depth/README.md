# Present-depth observer (host contracts)

`python3 tests/nr-present-depth/run.py`

Tests the production portable ranking, refusal, recording and window headers under ASan/UBSan:

- R1 last writable binding and R2 direct-draw count, independent of each other's decision;
- shadow/array/extent/sample exclusion, 2:1 margin, direction, state and every refusal;
- creation state, valid/mismatched/partial transitions, clears, heap aliases and cross-queue observations;
- invalidated lifetime generations, bounded capacity, replay/reset/close and coalesced draws;
- separate recorder threads and atomic descriptor snapshots under concurrent reads;
- independent eligible-comparison denominators, separate ring commits, both lines, A/B alternation,
  first 2-second window, changed/unchanged windows, 30-second heartbeat, empty and backwards-clock cases.

Ten seeded behavioral mutations must compile and fail the tests. Source guards check the startup gate, config
round trip, existing submission seam, installation/teardown, exact interface-signature assertions, separate
clock and absence of GPU commands or model/ring mutation in the adapter. When MinGW is installed, the whole
adapter is syntax/type-checked against its real D3D12 interfaces with minimal Config/State/Logger/ResTrack and Detours declarations;
no DLL is linked and no Windows binary runs. That optional result is reported separately.

This does not execute Detours, a real D3D12 object, an NGX model, Wine, a game or a shader. It cannot establish
hook coexistence, native/Proton correctness, heuristic precision, image quality or performance. The integration
build and the game's DLAA/no-upscaler measurements remain required. See
[the design](../../docs/dlssnr/design/present-depth-without-upscaler.md).
