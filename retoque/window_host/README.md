# RetoqueWindowHost: NR on any window

`RetoqueWindowHost.exe` captures one top-level window of another program with Windows Graphics Capture and shows it,
with DLSS Neural Rendering applied, in a preview window of its own or an opt-in click-through overlay. Nothing is injected into the captured program. NR
is the production `OptiScaler.dll`'s present pass, reached through the gated `RetoqueWindow_*` exports
([DlssNr_WindowHost.cpp](../dlssnr/DlssNr_WindowHost.cpp)), so every present-pass setting and fix applies.

Design, decisions and status: [nr-anything.md](../../docs/dlssnr/design/nr-anything.md). **Windows 10 2004 or later,
an NVIDIA GPU, the user's own NR model. In progress, not qualified: built and host-tested on Linux, never run on
Windows yet.** Phases 0-2 have a historical MSVC build; phase 3 is implemented and its host test binary is built/tested,
with MinGW syntax checks of the Win32 units. The integrated MSVC build and native run are still pending. No app screen
(phase 4), upscaling or depth estimation; capture stays SDR.

## Files

| File | What |
|---|---|
| `Main.cpp` | Command line, the frame loop, teardown and the restart-required path |
| `Capture.cpp` | WGC: item, free-threaded pool, newest frame wins, frames held until their copy's fence completes |
| `Transport.cpp` | D3D11 to D3D12: three shared NT-handle textures, two shared fences, GPU-side waits only |
| `Output.cpp` | Preview HWND / overlay DComp output on the D3D12 queue; copy into the backbuffer, geometry and visibility |
| `NrSeam.cpp` | Sets `RETOQUE_WINDOW_HOST=1` in this process, loads `OptiScaler.dll` from the exe's folder, checks the version |
| `Sources.cpp`, `Devices.cpp` | Window facts, identity, catalog, crop inputs, the adapter on the source's monitor; the devices |
| `Geometry.h`, `Policy.h`, `Overlay.h`, `Pacing.h`, `Protocol.h` | Pure rules, no Windows types, held by `tests/nr-window-host-rules` |
| `window-host.ini` | The `OptiScaler.ini` for the host's folder |

Build: `./build-local.sh Release` builds `window_host.vcxproj` after the forwarder and the DLL and publishes
`x64/out/RetoqueWindowHost.exe` with them. Every translation unit starts with this folder's `pch.h`; the pure headers
never include it.

## The host's folder

One folder of its own, never a game's: `RetoqueWindowHost.exe`, `OptiScaler.dll`, `nvngx.dll_dlssnr.dll` (all three
from the same build), `window-host.ini` renamed `OptiScaler.ini`, and the user's model as `nvngx_dlssnr.dll` or named by
`[DlssNr] ModelFile`. The model is never shipped. `OptiScaler.log` is written there.

## Command line

```text
RetoqueWindowHost --catalog
RetoqueWindowHost --run --window 0xHANDLE [--max-frames N] [--trace] [--no-nr] [--no-vsync]
RetoqueWindowHost --run --countdown SECONDS  (1-60; takes the foreground window when it ends)
RetoqueWindowHost --run --countdown 5 --overlay [--exclude-from-capture]
```

`--run` still defaults to preview. `--overlay` selects an owned, non-topmost, never-activated popup over the source's
client area; the real cursor is used. **Ctrl+Alt+F10 stops the overlay**, even while another app has focus. If that
hotkey cannot be registered the overlay refuses to start. `--no-nr` uses either output without loading the DLL (a
control, and the route on a non-NVIDIA adapter).

The overlay hides while the source is disabled, has a visible owned popup/dialog, enters a menu or move/size loop,
is hidden/minimized/cloaked, or its state cannot be read. Focus loss alone does not pause: other apps may cover the
owned output. Geometry changes and crop mismatches hide it too. It appears only after a fresh successfully presented
frame matches the current physical client rectangle. Queued frames from before a pause/move cannot reveal it.

`--exclude-from-capture` requests `WDA_EXCLUDEFROMCAPTURE` on the overlay only. **Default off; the owner's recorder
policy remains undecided.** A failure to apply a requested exclusion refuses startup. There is no game INI key.
Overlay requires cursor-capture control and effective per-monitor-v2 DPI awareness. A wrapped swapchain detected by
the DLL seam stops with its refusal instead of continuing unprocessed. A Windows exclusive-D3D fullscreen signal
refuses the run, including if another program caused it; an unknown query pauses. Ordinary borderless fullscreen is
not rejected by size. Topmost sources are refused (`source-topmost`), because an owned popup can inherit topmost;
the host never demotes the source. These detection limits need native qualification.

| Exit | Meaning |
|---|---|
| 0 | Stopped: a command, the preview closed, `--max-frames`, or the source closed |
| 2 | The command line was refused |
| 3 | No Windows Graphics Capture or WinRT |
| 4 | The window may not be captured, or its crop could not be confirmed for a second |
| 5 | `OptiScaler.dll` missing, without the exports, another seam version, or refusing the session |
| 6 | A device, the transport or the preview could not be created |
| 7 | `restart-required`: GPU completion could not be proved or the device was lost; everything was abandoned |

## JSON lines

stdout carries one object per line, each with an `"event"`: `window` and `catalog` (`--catalog`); `countdown`,
`started`, `stream` (a new stream and why), `resized`, `paused`, `resumed`, `nr` (when the pass's status changes: entered,
running, passes, status), `frame` (with `--trace`), `status`, `command-refused`, `refused` (with `stage`, `reason` and
`exit`), `restart-required` and `stopped` (with the counters and `age_p50_ms`/`age_p95_ms`, the compositor's stamp to
the host's Present). Every run event adds `"output":"preview"` or `"output":"overlay"`; `started` also reports
`exclude_from_capture` and `disengage_hotkey`, and `status` reports `overlay_visible`. stdin takes `{"cmd":"status"}`, `{"cmd":"stop"}` (or `"quit"`), each optionally with
`"rev": N`, which `status` echoes. Unknown commands are refused by name and never acted on. No network listener.

## Lifetime

A GPU object is released only after the fence proving its last use completed. The CPU waits on a fence only before a
resize and at exit, for at most 2 s; otherwise it blocks only on the swapchain's frame-latency object. A timeout or a
lost device ends the process with exit 7 without releasing anything. `OptiScaler.dll` is never unloaded and
`RetoqueWindow_End` frees nothing: the pass keeps what it built until the process exits, as in a game.

## Provenance

The capture, crop, candidate, interaction and overlay mechanics re-derive NeuRotic `beae4eea` (`apps/NeuRoticWindowWorker/capture/`), which
adapted Magpie `69d6105d`; both GPL-3.0. See [Licenses/NRAnything_ATTRIBUTION.txt](../../Licenses/NRAnything_ATTRIBUTION.txt)
and [Licenses/Magpie-LICENSE.txt](../../Licenses/Magpie-LICENSE.txt). NeuRotic's NR host, forwarder ABI, guidance and
shaders are not used.
