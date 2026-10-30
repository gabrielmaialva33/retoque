# Shared D3D12 upscaler menu lock

Run `python3 tests/upscaler-menu-lock/run.py` with Python and g++.

With the legacy menu (`OverlayMenu=false`, e.g. the RE Engine quirk) every D3D12 upscaler feature shares one static
`Menu_Dx12`. A replaced or failed feature is destroyed on its own thread two seconds later (`Util::DelayedDestroy`)
and its destructor resets that menu. The suite compiles `IFeature_Dx12::Init`, `RenderLegacyMenu`, the destructor and
the header's menu declarations verbatim under ASan/UBSan, against a fake menu that is poisoned and freed when
destroyed, and races:

- a destructor on another thread while the running feature draws the menu (must wait for the draw; the next draw
  recreates the menu as before);
- two destructors at once, 200 rounds (no double free, no menu left);
- a feature creating the menu in `Init` while another is destroyed, 200 rounds (count and pointer agree).

Source guards: every use of `Imgui` in `IFeature_Dx12.cpp` is in those three functions, each takes `ImguiMutex`
before touching it, and the header declares the lock beside the menu with its lock order.

Limits: the draw race is deterministic; the other two are probabilistic before the fix and deterministic after it.
No ImGui, D3D12 or game runs. The menu's own GPU lifetime (its heaps and render targets released on reset) is not
changed or covered here.
