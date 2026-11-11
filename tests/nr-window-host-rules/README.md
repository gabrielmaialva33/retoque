# nr-window-host-rules

NR on any window, phase 2 ([design](../../docs/dlssnr/design/nr-anything.md)): the capture host's pure rules
(`retoque/window_host/{Geometry,Policy,Pacing,Protocol}.h`), compiled as they ship with g++ and sanitizers:

- client crop against DWM's frame bounds, confirmed by the captured size: framed, borderless, negative coordinates,
  a stale size after a resize, Magpie's two maximized cases, no DWM frame, and the refusals;
- the candidate filter (and its paused variant), display affinity refused, source identity, run/pause/stop, the
  adapter rule;
- pacing: QPC to 100 ns without overflow, strictly later stamps, new streams (first, resume, geometry, a gap over
  100 ms), the one-second crop wait, nearest-rank source-age percentiles over a bounded window;
- the command line, the JSON event encoding and the stdin command parser, with its refusals.

Plus a source contract on the Windows units: `pch.h` first, the pure headers free of Windows headers, devices and the
preview swapchain created before `OptiScaler.dll` is loaded and no factory, device or swapchain created after it, the
variable set before `LoadLibraryExW`, no `FreeLibrary`, the DLL's own `DlssNr_WindowHost.h` used for the outcome, and
the NeuRotic/Magpie attribution present. Nothing here runs on Windows.
