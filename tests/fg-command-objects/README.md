# FG D3D12 command objects and HUD-less format

Run `python3 tests/fg-command-objects/run.py` with Python and g++.

Ports from lonewolf0622/HelixSR-Scaler (GPL-3.0), verified against this tree. The suite compiles, verbatim and under
ASan/UBSan, the command-object members of `IFGFeature_Dx12`, its UI/SC list getters and
`DropIncompleteCommandObjects`, both `DLSSG_Dx12::CreateObjects` and `XeFG_Dx12::CreateObjects`, and the HUD-less
format gate from both backends' `SetResource`.

- 01b95a7d97: a device failing at each of the 18 creation calls in turn leaves no partial set behind (only the shared
  fences survive), the next request on a healthy device builds the whole set, and every object is released once.
  A getter refuses a slot whose list or allocator is missing instead of calling through it.
- 1c90e142e8: a HUD-less format change is refused once and restarts frame generation; the new format is then
  accepted, without a restart every frame.
- 49a25a3d9a (source guard): `DLSSG_Dx12::SetResource` records no barrier on the game's input. The removed restore
  named COPY_DEST as the before-state of an input still in COPY_SOURCE and could run on a null command list.
- fba8eb77d7 (applicability guard): releasing the shared UI/SC fences and events on destruction does not apply while
  FG objects live for the process. The guard fails if production code deletes or clears `currentFG`, if a
  backend's `ReleaseObjects` touches the shared fences, or if `CreateObjects` stops reusing them; port that fix
  first then, releasing a fence only once its completion is proven.

Limits: fakes only. Barrier correctness, Streamline/XeFG acceptance and GPU timing need the D3D12 debug layer on
native Windows and a game with HUD-less input.
