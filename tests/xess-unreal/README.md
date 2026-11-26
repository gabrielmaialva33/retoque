# XeSS Unreal Engine hardening

Run `python3 tests/xess-unreal/run.py` (Python 3, g++ with ASan/UBSan).

The production `XeSS_UE_Decode.h` is compiled directly. Fixtures cover both MOV encodings, all 16 source and
destination registers, narrow/wide writes, subsequent field loads, negative/unaligned displacements, first-field
selection, stores and LEA. The runner extracts the actual `InitCopy`, `RequestCopy`, `HookModule` functions and
`Hook`'s pre-scan gate and compiles them against fake D3D12/Detours/scanner interfaces. It checks success and
commit failure for both plugin shapes, with/without the optional view-size hook, copy-hook failure, same/foreign
devices, failed GetDevice, and the off switch. Mutations demonstrate that each rollback assignment, device check,
startup gate and the narrow-MOV fix are exercised. Config declaration/read/save/INI/docs are source contracts.

These are host checks, not real Detours installation, UE scanning, GPU synchronization, a DLL build or game/native
Windows qualification. Fakes deliberately do not emulate D3D12 lifetime or instruction walking. Full source bodies
are used for the hook/copy decision tests; the scan loop following the startup gate is outside the fixture.
