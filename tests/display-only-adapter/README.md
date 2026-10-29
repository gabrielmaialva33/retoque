# Display-only adapter

`python3 tests/display-only-adapter/run.py` compiles `retoque/misc/DisplayOnlyAdapter.h` against a fake D3DKMT under
ASan/UBSan and checks, by text, that `IdentifyGpu`'s DXGI enumeration (`OptiScaler/misc/IdentifyGpu.cpp`) skips such an
adapter before it stores one.

An indirect display adapter (Parsec's virtual display, a streaming or remote-desktop display driver) is enumerated by
DXGI with the name and ids of the GPU it renders with, but cannot render, and NVAPI does not know its LUID. On Rafael's
RTX 3060 one came first in DXGI's order (`adapters.exe`, 2026-10-29: LUID 1F882, `IndirectDisplayDevice 1`,
`RenderSupported 0`, ahead of the real GPU at 1198A), so it could become IdentifyGpu's "primary GPU" with no NVIDIA
architecture and no DLSS; in his game logs it showed up as a second "RTX 3060" with `dlss: false`.

Cases: the Parsec shape is display-only; a render adapter and an indirect display that renders are kept; an adapter
that cannot be opened, a refused query (which still closes the adapter) and missing gdi32 entry points keep the adapter,
as before.
