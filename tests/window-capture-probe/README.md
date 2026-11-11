# window-capture-probe

NR on any window, phase 0 ([design](../../docs/dlssnr/design/nr-anything.md)): what a Windows (or Wine/Proton
prefix) offers a window-capture host, as a table of HRESULTs. `probe.cpp` checks WinRT, Windows Graphics Capture
support and its cursor/border APIs, the loaded `dxgi.dll`, D3D11 (with shared fences) and D3D12 devices, one captured
frame of a window it paints itself (centre pixel compared), display affinity, one frame of a target window, the
overlay's parts (click-through popup, `WDA_EXCLUDEFROMCAPTURE`, composition swapchain on a D3D12 queue, DComp target,
one presented frame, the queue drained) and DXGI desktop duplication. It never loads OptiScaler, NGX or a model.

`run.py` (wine tier) checks the source and **builds** `artifacts/probe.exe` with the MSVC kit under the shared
compiler-prefix lock. It does not run it: a PASS means "built", not "capture works". Run it by hand:

```text
probe.exe [--window 0xHANDLE] > probe-windows.txt              (native Windows 10 2004+)
python3 tests/window-capture-probe/run.py --parse probe-windows.txt
```

`--parse` prints the Markdown table and writes `artifacts/<name>.json`. Exit 0 means no check failed. Results go into
the design note as dated, measured facts.
