# FG fullscreen hooks

`python3 tests/fg-fullscreen-hooks/run.py` slices the FG swapchain's fullscreen hooks verbatim from
`OptiScaler/hooks/FG_Hooks.cpp` and compiles them against fakes under ASan/UBSan. Each case runs in its own
process, so one crash reports as one failure.

`hkGetFullscreenState` and `hkGetFullscreenDesc` (attached for XeFG) report the fullscreen that
`[XeFG] ForceBorderless` emulates. DXGI declares both `GetFullscreenState` arguments optional: the cases pass each
combination of NULLs and check that the original receives them as given, that nothing is written through NULL,
and that a reported fullscreen comes with an output (the window's, AddRef'd) instead of NULL, without replacing or
leaking a target the chain itself returned. Outside the emulation the real answer passes through untouched.

The fakes stand in for DXGI and for the window; this proves the hooks' argument handling, not what a game, XeFG or
the display does with the answer.
