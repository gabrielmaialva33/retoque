# FG fullscreen hooks

`python3 tests/fg-fullscreen-hooks/run.py` slices the FG swapchain's fullscreen hooks verbatim from
`OptiScaler/hooks/FG_Hooks.cpp` and compiles them against fakes under ASan/UBSan. Each case runs in its own
process, so one crash reports as one failure.

`hkGetFullscreenState` and `hkGetFullscreenDesc` (attached for XeFG) report the fullscreen that
`[XeFG] ForceBorderless` emulates. DXGI declares both `GetFullscreenState` arguments optional: the cases pass each
combination of NULLs and check that the original receives them as given, that nothing is written through NULL,
and that a reported fullscreen comes with an output (the window's, AddRef'd) instead of NULL, without replacing or
leaking a target the chain itself returned. Outside the emulation the real answer passes through untouched.

`hkSetFullscreenState`, `hkResizeTarget` and the window cover at the end of both resize hooks apply
`[XeFG] ForceBorderless` to XeFG only, as swapchain creation, the resize flags, the menu and the game configs do.
With the key left true while FSR-FG, DLSS-G or Reprojection runs (the menu saves it from the XeFG card, which is
hidden for the other outputs), the cases check that a chain created exclusive is taken out of it on `FALSE`, that
`TRUE` and `ResizeTarget` are forwarded, that the D3D11 bridge's take-back of a presenter DXGI put into exclusive
fullscreen reaches it without restyling the bridge's window, and that a whole mode-change script traces the same
with the key on and off. Controls: XeFG with the key still emulates (never forwarded, window styled both ways),
the key off forwards for every output, and an active FG is paused by a mode change either way.

The fakes stand in for DXGI and for the window; this proves the hooks' argument handling, not what a game, XeFG or
the display does with the answer.
