# NR Anything: NR on any window, by capture, with no hook in the game

Design study, 2026-11-11. Retoque baseline `3c53db9a`; NeuRotic Alpha 0.9.9 `beae4eea` (a local read-only clone,
GPL-3.0). NeuRotic paths below are relative to that clone's root. Retoque paths are relative to this repository. Line
numbers are at the two commits named above.

**Status (2026-11-11): in progress, not qualified.** The owner answered the open questions the same day
([Decisions](#decisions-2026-11-11)) and phases 0, 1 and 2 are implemented ([Implementation](#implementation-phases-0-2)):
the capture probe, the gated `RetoqueWindow_*` export in the production DLL, and `RetoqueWindowHost.exe` with its
preview window. All of it builds here (MSVC under Wine) and its pure rules and the DLL seam pass host tests. **None of
it has run on Windows, on a GPU or with the model**; the first native run on the RTX 3060 is described below. The app
screen (phase 4) and the overlay (phase 3) do not exist yet.

## The answer

NeuRotic's "NR Anything" is a separate Windows process. It captures a chosen top-level window with Windows Graphics
Capture (WGC) and runs the NR model on its own D3D12 device with zero (neutral) guides. It shows the result in a
click-through DirectComposition window that sits exactly over the source's client area. The game is never
hooked. It needs Windows 10 2004 or later on an NVIDIA GPU. Under Proton it cannot work as written: Wine's
`graphicscapture.dll` provides only `GraphicsCaptureSession.IsSupported`, and a worker in another prefix cannot see the
game's windows.

For Retoque the recommendation is option B. Write a small capture host of our own and run the NR pass through the
**production** `OptiScaler.dll`, using a fork-owned export modelled on the `RetoqueReplay_*` seam. Do not port
NeuRotic's worker. Our present pass already does what the worker would need, and does more: cold NR on zero guides,
GPU `SynthMotion`, `UiProtection`, `WorkingScale`, HDR present, `Compare` and the model loaders. Their worker would
bring a second NR host with its own forwarder ABI, its own composition shader and its own model-identity rules, and
that host would drift from ours. Take their capture and overlay mechanics as GPL-3.0 references, with Magpie's
attribution. Ship it **for Windows only**. On Linux, keep the in-process present hosts, which already cover Proton
titles, and show no NR Anything control.

## Decisions (2026-11-11)

The owner's answers to the [open questions](#open-questions-for-the-owner):

1. **Windows only.** NR on any window ships for native Windows. Linux and Proton get no control for it (the in-process
   present hosts already cover Proton titles); the app screen does not exist there.
2. **The seam, not the code.** The host drives the production `OptiScaler.dll` through a fork-owned export modelled on
   `RetoqueReplay_*`: `RetoqueWindow_Version/Begin/Present/End` in `retoque/dlssnr/DlssNr_WindowHost.cpp`, exported
   outside `Source.def`, **inert unless `RETOQUE_WINDOW_HOST=1`** is in the process environment. Only the capture
   program sets it, in its own process, before it loads the DLL; a game never does, and nothing in the DLL calls the
   exports.
3. **Version 1 scope: no upscaling and no depth estimation.** NR runs on the captured frame at its own size with
   neutral guides, using what the present pass already has: constant depth and zero motion, or `SynthMotion`'s
   estimate, `UiProtection`, `ZeroGuideReset` and `WorkingScale`, as the host folder's `OptiScaler.ini` sets them.
   Upscaling stays the first follow-up (phase 5); depth estimation stays a separate, later proposal.
4. **Name.** The app screen (phase 4, not part of this change) is "NR em qualquer janela" / "NR on any window".

Still open: whether the phase 3 overlay hides itself from recorders (`WDA_EXCLUDEFROMCAPTURE`, question 3) and whether
the app shows a notice about NGX evaluating arbitrary desktop windows (question 5). Neither affects phases 0-2: the
preview window is an ordinary window.

## What NeuRotic does

### Process model and IPC

- **A separate worker process.** `NeuRotic.WindowWorker.exe` is built from `apps/NeuRoticWindowWorker/`
  (`WorkerMain.cpp:398-450`). The desktop Hub launches it suspended inside a job object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, then resumes it (`apps/NeuRoticHub/anything/AnythingController.cpp:129-143`).
  The command line is `--control`, plus `--host-pid <hub pid>` so that the Hub's own windows can never be selected
  (`:139`; enforced in `WorkerMain.cpp:179`).
- **IPC is JSON lines.** The Hub writes commands to the worker's stdin through an overlapped named pipe
  (`\\.\pipe\NeuRoticAnything-<pid>-<n>`, `AnythingController.cpp:123-127`) and reads events from anonymous stdout and
  stderr pipes (`:121-122`). The worker reads stdin on a thread into a queue of at most 16 commands of at most 64 KiB
  each (`WorkerMain.cpp:34-60`) and writes one JSON object per line (`:30`). There is no network listener (help text,
  `:391`).
- **Commands:** `status`, `catalog`, `verify-model`, `set-model` and `import-model`, `start` (by countdown or by a
  selected window), `stop`, `cancel`, `set-comparison`, `set-processing`, `snapshot` and `quit`
  (`WorkerMain.cpp:231-302`). Every request carries a revision, and status events report what was requested and what
  was applied. Status is sent every 100 ms while the worker is active.
- **Window selection.** One way is a countdown: the user switches to the target, and the foreground window is taken
  when the countdown ends (`WorkerMain.cpp:259-263, 307-316`). The other is a catalog from `EnumWindows`
  (`capture/CaptureHost.cpp:351-360`). The catalog excludes invalid or non-root windows, the desktop and shell,
  hidden, minimized, cloaked, tool, owned-popup and transparent windows, windows under 50 px, and the worker's own
  windows (`:38-70`). Identity is the HWND, PID, process creation time, class and executable, and it is re-checked on
  every frame (`:74-80`).
- **The same worker also runs inside a game.** `[DlssNr] Route=3` ("NR Anything", `OptiScaler.ini:1574-1582`)
  turns off the in-game NR runtime (`OptiScaler/Config.cpp:94`). The game DLL then launches
  `OptiScaler/NRAnything/NeuRotic.WindowWorker.exe` from beside itself and has it capture the game's own window
  (`OptiScaler/dlssnr/RenderingOutput.cpp:260-312`). It uses the shared `AnythingController` compiled into the game DLL
  (`:20-40`) and the model it finds beside the DLL or the executable (`:295-297`). While the in-game menu is open it
  shows the original (`split=1`, `:311`).

### Capture

- **WGC, per window.** `IGraphicsCaptureItemInterop::CreateForWindow` (`CaptureHost.cpp:390-392`) feeds a free-threaded
  frame pool with two buffers (`:402`). The pool is BGRA8, or RGBA16F scRGB when the observed desktop colour is HDR
  (`:121, 280-301`). The `FrameArrived` callback only signals a wake event (`:405`), and the copy runs on the owner
  thread.
- **Newest frame wins.** Each poll takes at most two frames and drops the older one (`:433-438`). If the content size
  changes, the pool is recreated (`:441-443`).
- **Client crop.** The client area is mapped against DWM extended frame bounds, with Magpie's correction for
  maximized windows. If the WGC size cannot confirm the crop origin, frames are dropped for up to 1 s and the session
  then fails (`:81-111, 444-452`).
- **Clock.** `SystemRelativeTime` is converted to QPC (`:456-461`). It drives the frame stamp, the history reset when
  there is a gap of more than 100 ms, and the "source age" reported in status.
- The cursor is captured only in preview mode. In the overlay the real cursor is visible, so a delayed copy is never
  drawn (`:408-411`). Without `IsCursorCaptureEnabled` the overlay refuses to start (Windows 10 2004 or later).
- **Provenance.** "Magpie GPL v3 source adaptation … Adapted from Magpie 69d6105d" (`:2-3`). The HDR colour and
  snapshot helpers cite Microsoft's Win32CaptureSample (MIT).

### NR evaluation in the worker

- **Its own D3D12 device on the capture adapter.** `D3D12CreateDevice` uses the adapter of the D3D11 capture device
  and refuses to run if the LUIDs differ (`nr/NrHost.cpp:574-596`). It has a private direct queue, allocator, list and
  fence.
- **D3D11 to D3D12 transport.** The worker copies into a D3D11 texture shared with D3D12 through an NT handle,
  signals a shared `ID3D11Fence`, and the D3D12 queue `Wait`s on that fence (`:505-528, 610-617, 738-751`). The result
  returns through a second shared texture. A tiny D3D11 blit, compiled at runtime with `D3DCompile`, converts it to a
  BGRA8 output slot (`:619-624, 755-801`).
- **Model and forwarder.** The user's `nvngx_dlssnr.dll` must have exactly that name (`:642-643`). The forwarder,
  `nvngx.dll_dlssnr.dll`, sits beside the worker, and its SHA-256 is compiled into the worker
  (`WindowNrForwarderIdentity.h`, checked at `:644-650`). The worker loads the forwarder and binds NeuRotic-only
  exports: `dlssnr_call_evaluate_guides_v4`, `dlssnr_call_release_checked_v1`,
  `dlssnr_call_host_recorded_contract_v1` and `dlssnr_call_probe_d3d12` (`:661-680`). Our forwarder has none of
  these (`retoque/dlssnr/forwarder/dlssnr_forwarder.cpp` exports `dlssnr_call_create`, `dlssnr_call_evaluate`,
  `dlssnr_call_release` and the rest).
- **NGX core.** The path comes from the driver's registry key `HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm\NGXCore`
  value `NGXPath` (`:683-700`); the worker loads `nvngx.dll` or `_nvngx.dll` from that path (`nr/NrPolicy.cpp:92-100`).
  It then calls `NVSDK_NGX_D3D12_Init_Ext(0x24480451, …)` itself (`:701-716`), the same application id that Retoque's
  `DlssNr_DirectRuntime.cpp:550` uses. The parameter ABI is probed with float slots (`:718-735`).
- **Per frame.** A NeuRotic composition shader (`OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader.h`, which
  includes RenoDX-attributed composition) runs a proxy dispatch, an optional reduce to the working extent, the model
  evaluate, and a compose back to the source size (`:860-950`). The model is created with the working extent and
  `modelStyle` 0, 1 or 2 (Standard, Natural, Cinematic) (`:556-558`).
- **Fully serialized.** Each stage waits on the CPU before the next one starts: the copy is drained
  (`CaptureHost.cpp:475-477`), the NR submission is drained with a 5 s timeout (`NrHost.cpp:231-280`), the D3D11
  publication is waited on (`:790-797`), and the present is drained (`CaptureHost.cpp:531-538`). Nothing is
  pipelined. Any completion that cannot be proved poisons the generation.

### What replaces depth and motion

- **Default: none.** With `guides=off` (the Hub's and in-game default; `RenderingOutput.cpp:310`), depth and motion are
  cleared to zero on every frame (`NrHost.cpp:443-466, 863`). The README states plainly that window capture "does not
  receive native game depth or motion vectors" and that "depth estimation [is] unavailable in this package".
- **Experimental guides, off in shipped builds.**
  - CPU block-match motion: a staging readback, area-filtered luma, and an 8x8 SSD search over ±12 px
    (`guidance/ImageMotion.h`, `GuidanceHost.cpp:181-191`).
  - Depth Anything V2 Small through ONNX Runtime and DirectML, from a pinned local bundle (`model.onnx`,
    `DirectML.dll`, `onnxruntime.dll`; `guidance/DepthBundle.h:88`, `DepthRuntime.h:76-85`).
  - An optional TensorRT 10 / CUDA 12 backend (`guidance/dav2/Dav2Backend.cpp`). The user must build its engine cache
    locally from official NVIDIA runtimes (`tools/depth-anything-v2/README.md`).

  Their own README says estimated depth "is not yet bound to the NR model".

### Output

- **The overlay.** The output is a `WS_POPUP` with `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
  WS_EX_TOOLWINDOW`. It is **owned by the source** and is not topmost, so it stays just above the source and
  unrelated foreground windows still cover it (`CaptureHost.cpp:306-307, 221-222`). It answers `HTTRANSPARENT` and
  `MA_NOACTIVATE` (`:203-204`).
- **Swapchain.** `CreateSwapChainForComposition` with flip-sequential, two buffers, presented through a DComp
  target and visual (`:316-331`). The format follows the capture's colour representation (`:316, 321`).
- **Hidden from capture.** `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` (`:311`), so screen recorders show the
  unprocessed source.
- **Follows the source.** Every frame, the window is moved to the source's client rectangle in screen coordinates
  (`:213-225`). It is hidden on a size mismatch, and made visible again only after a matching frame has been accepted
  (`:334-335, 539-550`).
- **DPI and monitors.** The process is per-monitor-v2 DPI aware (`WorkerMain.cpp:399`). Colour is observed per output,
  and a change drains the pipeline, recreates the pool and bumps the epochs (`CaptureHost.cpp:280-301`). There is no
  other multi-monitor logic.
- **One adapter.** Capture, processing and output must all use the same adapter (`WorkerMain.cpp:201-206, 220`;
  adapter ranking in `capture/ProcessingAdapter.cpp:92-123`).
- **Preview mode.** An ordinary `WS_OVERLAPPEDWINDOW` with an HWND swapchain (`:305-307, 319`).

### Input pass-through and pause policy

- **Input reaches the source.** Clicks and keys go to the game because the overlay cannot be hit-tested or activated.
  A starting overlay hands the foreground to the source once (`WorkerMain.cpp:183-187`).
- **Pauses.** Capture pauses and the overlay hides in these cases:
  - the source is disabled, has an owned dialog, or is in menu or move-size mode (`capture/InteractionPolicy.h`);
  - it is hidden, minimized or cloaked (`CaptureHost.cpp:172-189, 564-583`).
- **Disengaging.** Ctrl+Alt+F10 is registered as a hotkey that disengages the overlay, and so are Escape and
  `WM_CLOSE` on it (`:205-206, 309`).

### Pacing, latency and resolution

- **Scheduling.** Scheduling is event driven: `MsgWaitForMultipleObjectsEx` waits on the capture wake, the command
  queue and window messages, with a 50 ms maintenance tick (`control/WorkerWake.h:19-26`, `WorkerMain.cpp:125-131`).
- **Presentation.** The output is presented with `Present(0, DXGI_PRESENT_DO_NOT_WAIT)` (`CaptureHost.cpp:534`). There
  is no vsync wait and no latency-waitable object, and the frame rate is capped by the serialized chain above. The
  status reports per-stage CPU times, GPU timestamps, and p95 source age from the compositor clock to the end of the
  present.
- **Resolution.** The NR working scale is 25-100%, resolved with the same aligned policy as their in-game Present
  route (`nr/NrPolicy.cpp:35-46`). The output is always the source's client size: no upscaling, and no frame
  generation (README: "desktop super resolution, frame generation … unavailable").
- **Admission budget.** Admission is capped at 1280 MiB of owned memory for SDR and 1536 MiB for HDR. Opaque driver
  and model memory is not counted (`NrPolicy.cpp:69-90`).

### Failure handling

- **Quarantine.** Any completion that cannot be proved quarantines the generation. COM and WGC objects are
  deliberately leaked for the rest of the process (`CaptureHost.cpp:340-346, 585-587`). The worker then reports
  `RestartRequired` (`WorkerMain.cpp:164-176`), and the Hub kills and relaunches it, restoring the model
  (`AnythingController.cpp:213, 242`).
- **Other stops.** A source that closes, changes identity, or outlives the 1 s crop wait stops the session.

### Windows-only surface, licence, maturity

- **Windows only.** The worker depends on Windows throughout: WGC and `IsCursorCaptureEnabled`, DirectComposition,
  `WDA_EXCLUDEFROMCAPTURE` (Windows 10 2004+), the NGXCore registry path, job objects, named pipes and
  `SHGetKnownFolderPath`. The help text says "Windows 11 x64" (`WorkerMain.cpp:383`).
- **Licences.**
  - NeuRotic is GPL-3.0 (`LICENSE`), which is compatible with ours.
  - The capture code is adapted from Magpie (GPL-3.0, `Licenses/Magpie-LICENSE.txt`,
    `ATTRIBUTION.md:17`).
  - The colour helpers come from Win32CaptureSample (MIT).
  - The composition shader carries RenoDX attribution.
  - The Depth Anything packet is MIT (`guidance/dav2/PACKET-LICENSE`). The model and its source are Apache-2.0.
    CUDA, TensorRT, ONNX Runtime and DirectML are vendor runtimes under their own licences.
- **Maturity, in their own words.** "Early alpha", "work in progress"; HDR is "experimental" (`README.md:119-121`,
  `RELEASE_NOTES.md:21, 86, 247-249`). No measured latency or quality results are published in the clone.
- **The worker size.** About 5,000 dense lines of C++ (capture ≈1.3k, control ≈0.8k, NR ≈1.3k, guidance ≈1.3k, main
  and contracts ≈0.7k), plus ≈1.7k for the Hub view and controller, excluding vendor headers.

## What Retoque already has

| Need | Retoque today | Reuse |
|---|---|---|
| NR on a frame that no upscaler produced | D3D12 present pass, `RunPresentPass(IDXGISwapChain3*, ID3D12CommandQueue*, …)` (`retoque/shaders/dlssnr/DlssNr_Dx12.cpp:4289`). It runs cold on `ZeroGuides` (`retoque/dlssnr/DlssNr_ZeroGuides.h`). The ini text names "a D3D12 title or emulator on HookMethod=2" | The core of option B. It takes a raw swapchain and a queue, not a wrapped one |
| Motion without an engine | `SynthMotion` (GPU FidelityFX estimator, optional NVOFA), `ZeroGuideReset` (`OptiScaler.ini:2039-2060`, [synthesized-motion](synthesized-motion.md)) | Better than NeuRotic's CPU block match, with no readback |
| HUD and text left alone | `UiProtection` (`OptiScaler.ini:2055-2063`) | Directly relevant to desktop windows (text, UI) |
| Working scale, cadence, looks, strengths | `WorkingScale`, the cadence modules, Transfer and Colour strength | Equivalent of their scale and strength controls |
| HDR | `HdrPresent` (HDR10 or scRGB to linear and back), experimental | Covers their scRGB capture case |
| Comparison | `Compare` side by side or wipe (`OptiScaler.ini:2399-2420`), [comparison-capture](comparison-capture.md) | Their split and stripes become our `Compare` |
| Model loading | Forwarder (`retoque/dlssnr/forwarder/`, fault containment) or `ModelLoader=direct` (`DlssNr_DirectRuntime.h`), the same NGX application id | Unchanged |
| A process other than a game driving the production DLL | `RetoqueReplay_*` exports, inert unless `RETOQUE_REPLAY=1` (`retoque/dlssnr/DlssNr_Replay.cpp:69-127`). `tests/dlssnr-loopback` loads `OptiScaler.dll` with `LoadLibrary` and runs cold NR and present NR under Proton with the real model (`harness.cpp:500-516`, `cold_nr.h`, `present_nr.h`) | The template for the seam and for the Phase 1 GPU proof |
| Desktop app | `tools/app` (Rust/egui, Linux and Windows): game discovery, receipts, per-game ini, a model catalog in the app's data folder with `[DlssNr] ModelFile` | Hosts the launcher on Windows. A model from the catalog, never copied into a game |
| Toolchain | The MSVC kit used by `build-local.sh` has SDK 10.0.26100 with `cppwinrt` and `windows.graphics.capture.interop.h` | A WGC host can be compiled here. Running it needs Windows |

What does **not** exist:

- no window capture and no overlay window;
- no host process;
- `WrappedIDXGIFactory7::CreateSwapChainForComposition` passes straight through without wrapping
  (`OptiScaler/wrapped/wrapped_factory.cpp:434-443`), so a composition swapchain never reaches the present hook by
  itself;
- `docs/capabilities.md` has no entry for capture.

## Linux and Proton feasibility

**Known, from inspecting the local Proton Experimental `experimental-11.0-20261105c` (and GE-Proton11-12) by string
search only:**

- **WGC.** `graphicscapture.dll` exists. The only `GraphicsCaptureSession` member it names is `IsSupported`. The DLL
  registers the runtime class names `GraphicsCaptureItem` and `Direct3D11CaptureFramePool`, but no
  `CreateForWindow` or interop symbol was found. NeuRotic refuses as soon as `IsSupported` is false
  (`CaptureHost.cpp:375`). Assume that WGC capture of another window does not exist under Wine **until a probe shows
  otherwise**.
- **DXGI duplication.** Wine's `dxgi.dll` contains the debug-channel names `dxgi_output_DuplicateOutput` and
  `DuplicateOutput1`. Upstream they are believed to be stubs. This is unverified here and needs the probe. DXVK's
  `dxgi.dll`, which Proton uses for D3D11 and D3D12 titles, showed no duplication strings.
- **Prefixes.** Each Steam game runs in its own prefix and wineserver. A worker started from the Retoque app's own
  prefix cannot enumerate or open the game's HWNDs. It would have to be launched inside the game's prefix (a launch
  wrapper, or from the game DLL as NeuRotic's `Route=3` does), and that gives up the "no hook" benefit.
- **Window contents in Wine.** Inside the game's prefix, a GDI read (`BitBlt` or `PrintWindow`) of a window that DXVK or
  vkd3d-proton presents through Vulkan WSI is not expected to contain the rendered image. The image goes to the X11
  or Wayland surface, not to a Wine-side surface. This is untested.
- **Capture on the Linux side.** It is possible natively: XComposite and SHM on X11 or XWayland, the
  xdg-desktop-portal ScreenCast with PipeWire on Wayland (with a consent prompt per session), or gamescope's PipeWire
  stream. But NR cannot run natively. [native-linux-route.md](native-linux-route.md) step 0 found
  no `libnvidia-ngx-dlssnr.so`. The frames would have to cross into a Wine process to run through vkd3d-proton.
  That is the same unproven memory and semaphore bridge as that note's option (a), plus an overlay. On Wayland, a
  client cannot place a click-through window over another client's window (no global positioning; layer-shell is not
  available on GNOME). On X11, an override-redirect window with an empty input shape can do it.
- **What Linux users already get.** Every Proton title already has in-process routes: the D3D12 present pass, the
  D3D11 bridge, and the Vulkan present host for DXVK and vkd3d-proton titles. They have lower latency than a capture
  path, with no extra copy and no extra compositor frame.

**Conclusion:** NR Anything is Windows-only. Any Linux variant would be a separate project behind
native-linux-route's gates. Per the boundary "no control for an unavailable feature", the app shows nothing for it on
Linux.

**What a test would settle** (Phase 0): in a Proton prefix, call `GraphicsCaptureSession::IsSupported()`,
`IGraphicsCaptureItemInterop::CreateForWindow` on a window of the same process, and
`IDXGIOutput1::DuplicateOutput` on the Wine and DXVK `dxgi`, and record the HRESULTs.

## Options

| Option | What it is | Gain | Cost and risk |
|---|---|---|---|
| **A. Port NeuRotic's worker** | Bring in `apps/NeuRoticWindowWorker` (capture, control, NR host) with its forwarder | Fastest route to a demo that matches theirs | A second NR host beside ours. It needs NeuRotic's forwarder ABI (`_guides_v4`, `release_checked_v1`, `host_recorded_contract_v1`) or a rewrite of `NrHost.cpp` against ours; their composition shader (≈1.5k-line precompiled header); `CanonicalProviderIdentity` and the other headers from their `OptiScaler/dlssnr/`. None of our `SynthMotion`, `UiProtection`, HDR present, `Compare` or fault containment, and every later NR fix would have to be made twice. Serialized CPU drains |
| **B. Our capture host plus the production DLL through a fork export** (recommended) | A small exe: WGC capture, D3D11→D3D12 transport, a composition overlay. It loads Retoque's `OptiScaler.dll` and calls `RetoqueWindow_Present(swapchain, queue)`, which runs `RunPresentPass` on the host's own swapchain | One NR implementation. Every present-pass feature and fix applies. The seam has a proven pattern (`RetoqueReplay_*`). It can be proved under Proton without capture, through the loopback harness | Unknown: how much of `RunPresentPass` assumes a wrapped game swapchain (`State` fields, FG checks), and which hooks `OptiScaler.dll` installs in a process it was loaded into as a library. Both are Phase 1 questions |
| **C. A presenter with Retoque as its `dxgi.dll` proxy** | The same host, with no export: it presents through an HWND swapchain that the proxy wraps (HookMethod=2) | No new DLL code at all | A wrapped swapchain cannot be a click-through composition overlay, so this is preview mode only. As a step it is useful (Phase 2 can use it to cross-check B). As a product it is not |
| **D. Port `Route=3` (the game launches the worker on itself)** | The in-game DLL captures its own window through a child worker | Covers games where in-process NR placement fails | It still needs the DLL inside the game, so there is no anti-cheat or "no hook" benefit. Our in-process present routes are already the better tool there. Not recommended |
| **E. A Linux-native capture variant** | PipeWire or X11 capture with Wine-hosted NR | Native Linux apps (emulators, players) | Blocked on native-linux-route's bridge and a native NR model. Out of scope |

## Recommendation

1. **Build option B, Windows only, in small shippable phases** (below). Phase 1 proves the seam on our own GPU
   fixtures before any capture code exists.
2. **Port no NeuRotic code wholesale.** Re-derive the WGC capture, client crop, interaction policy and overlay window
   from NeuRotic's `CaptureHost.cpp` and `InteractionPolicy.h`, and credit them in a code comment and in this note:
   NeuRotic `beae4eea`, which adapted Magpie `69d6105d`, GPL-3.0. Keep `Licenses/Magpie-LICENSE.txt`, and the
   Win32CaptureSample MIT licence if any colour helper is used. Those parts are reference-quality and short (≈700
   lines).
3. **Do not use their NR host, guidance, depth estimation or forwarder.** Guides come from our `SynthMotion` and
   `UiProtection`. Depth stays constant. Depth estimation stays a separate, later proposal. Their own notes say it is
   not bound to the model.
4. **Pipeline where they serialize.** The GPU waits on the shared fence (`queue->Wait`); the CPU blocks only on the
   frame-latency waitable object and on our existing `PresentSync` policy. Every lifetime rule in
   [DEVELOPMENT.md](DEVELOPMENT.md) still applies: a resource is retired only when its fence
   value is proved, and is abandoned if the device is lost.

## Phased plan

Each phase is independently shippable or abandonable, off by default, and changes nothing in a game process.

### Phase 0: probes (≈1 day; 2 files, ≈200 lines; nothing shipped)

- **Build** `tests/window-capture-probe/probe.cpp` and a `run.py`, as a Windows exe. It:
  - checks `IsSupported`;
  - captures one frame of a test window it creates itself, then of an external window;
  - reads `GetWindowDisplayAffinity` of the target;
  - creates the DComp overlay with a composition swapchain on D3D12;
  - checks `DuplicateOutput` on both `dxgi` implementations.
- **Proof.** HRESULT tables:
  - on Windows 11 with the RTX 3060, where the expectation is that everything succeeds;
  - under the local Proton, where the expectation is that WGC fails, and a record of exactly how;
  - in a second process in another prefix, where the expectation is that the HWND cannot be seen.
- The results go into the Linux section above as measured facts. If Proton surprises us, Linux is re-planned. If it
  does not, Linux is closed.

### Phase 1: the DLL seam (≈2-3 days; fork-owned, 3-4 files, ≈350 lines plus a test)

- **The DLL side.** Add `retoque/dlssnr/DlssNr_WindowHost.cpp/.h` with `RetoqueWindow_Version/Begin/Present/End`,
  exported unmangled outside `Source.def` as replay does. They are inert unless `RETOQUE_WINDOW_HOST=1`. There is one
  owner thread, and `Present` calls `RunPresentPass(swapchain, queue, false)`. This touches no upstream file, and the
  default path is byte-identical because the exports are unreachable without the environment variable.
- **Audit and decide.** Audit `RunPresentPass`'s dependencies on a wrapped swapchain and on `State`, and the hooks
  installed when the DLL is loaded as `OptiScaler.dll`. Decide whether the gate should also skip hook installation.
- **The test side.** Add `tests/dlssnr-loopback --window-host`. A Win32 window with a D3D12 HWND swapchain renders
  the existing moving scene into the backbuffer, calls `RetoqueWindow_Present`, and reads back.
- **Proof.**
  - Host tier: the export is inert without the variable, and its version and refusals behave as specified.
  - Wine tier, under Proton with the real model:
    - with `ApplyModel` off, the read-back is byte-identical to the input;
    - with it on, the picture changes and the pass reports success;
    - with `SynthMotion` on, the existing motion oracle holds;
    - a resize and a shutdown are clean.

  This does not prove Windows behaviour or capture.

### Phase 2: capture host, preview output (≈1 week; 6-8 files, ≈1,200-1,500 lines)

- **Files.** `retoque/window_host/` with its own vcxproj in the solution, built like the forwarder:
  - `Main.cpp`, the CLI (`--catalog`, `--run --window HWND | --countdown N`, `--max-frames`, `--trace`);
  - `Capture.cpp`, WGC with the client crop and identity re-check;
  - `Transport.cpp`, the D3D11 shared NT-handle texture and fence into D3D12;
  - `Output.cpp`, an ordinary window with a D3D12 HWND swapchain;
  - `Policy.h` and `Geometry.h`, pure and Windows-free: candidate filter, crop math, epochs, reset rules;
  - the README.
- **Configuration.** The DLL runs from a host-private folder with its own `OptiScaler.ini`: `HookMethod=2`, the model
  via `ModelFile`, and the user's choice of `SynthMotion` and `UiProtection`.
- **Proof.**
  - Host tests of `Policy.h` and `Geometry.h` (g++, no Windows types): crop cases including Magpie's maximized case,
    exclusion rules, and history-reset triggers.
  - A **native Windows run on the RTX 3060**: the loopback moving scene in windowed mode is the source.
    - With `ApplyModel` off, the output hash equals the cropped capture.
    - With it on, the output changes.
    - Resize, move across monitors, minimize, restore and closing the source all behave correctly.
  - Measurements: p50 and p95 source age (WGC `SystemRelativeTime` against present statistics), GPU time per stage,
    and VRAM delta.
  - A cross-check against option C on the same source.

### Phase 3: overlay (≈4-5 days; 2-3 files, ≈500 lines)

- **What is built.**
  - The DComp composition swapchain on the D3D12 queue.
  - An owned, non-topmost popup with `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`,
    answering `HTTRANSPARENT` and `MA_NOACTIVATE`.
  - It follows the source's client rectangle each frame, is shown only after a matching frame, and hides on a
    mismatch.
  - The pause policy (menus, dialogs, move-size, minimized, cloaked).
  - A disengage hotkey.
  - Per-monitor-v2 DPI awareness, and `WDA_EXCLUDEFROMCAPTURE` if the owner agrees (open question 3).
- **Proof.**
  - A host test of the interaction policy table.
  - On Windows on the 3060, a checklist:
    - clicks, keys and wheel reach the source;
    - Alt-Tab, another window covering the overlay, monitors with different DPI and the source losing focus all
      behave correctly;
    - a borderless fullscreen source is covered;
    - for an exclusive-fullscreen source, the outcome is recorded.
  - Latency against preview mode.

### Phase 4: app integration (≈1 week; Rust, ≈600-900 lines plus tests)

- **What is built.**
  - A Windows-only (`cfg(windows)`) "Any window" screen in `tools/app`. It spawns the host with JSON lines on
    stdin and stdout, in a job object that kills it on close. The protocol keeps NeuRotic's revision, requested and
    applied idea, but is our own.
  - Its controls: model from the catalog, window from the catalog or by countdown, strength, working scale,
    `SynthMotion`, `UiProtection`, `Compare`, Start and Stop.
  - The app stops the host before it updates the host's own files.
  - On Linux the screen does not exist.
- **Proof.**
  - `cargo test` with a fake host process that speaks the protocol: crash, garbage, overflow, restart-required.
  - Windows-gnu clippy.
  - A Windows manual run.
- An entry in `docs/capabilities.md` marked *Experimental, Windows only*, with its evidence links.

### Phase 5, optional: quality and HDR (size per item)

- scRGB capture with `HdrPresent`; a measurement on a real HDR display is required before any default changes.
- An upscaling mode: capture a window at a lower size and show it larger. NeuRotic lists this as future work.
- Depth estimation, only as a separate design note.

**Not planned:** porting `Route=3`, Linux capture, frame generation on the overlay.

## Implementation (phases 0-2)

Branch `feat/nr-anything-20261111`, 2026-11-11. Builds here; not run on Windows.

### Phase 0: the probe

`tests/window-capture-probe/probe.cpp` (one file) prints one `PROBE <check> <ok|fail|skip> hr=0x... <detail>` line per
check: the environment (Windows build, Wine version if any), WinRT, `GraphicsCaptureSession::IsSupported()`, the
cursor and border APIs, the DXGI factory and the loaded `dxgi.dll`'s path (to tell Wine's from DXVK's), the D3D11
device and its shared fences, the D3D12 device and queue, one frame of a window it creates and paints (the centre
pixel read back and compared), `GetWindowDisplayAffinity` on that window and on the target, one frame of the target
(`--window 0xHANDLE`, else the foreground window), the overlay's parts (a click-through popup,
`WDA_EXCLUDEFROMCAPTURE`, a composition swapchain on a D3D12 queue, a DComp target, one presented frame, the queue
drained), and `IDXGIOutput1::DuplicateOutput` plus one `AcquireNextFrame`. The suite `window-capture-probe` (wine
tier) builds it under the compiler lock and checks its source; it **does not run it**. `run.py --parse FILE` turns a
run's output into the HRESULT table. The Proton and cross-prefix rows of the proof are left to whoever wants Linux
re-planned; the owner closed Linux (decision 1).

### Phase 1: the seam

- `retoque/dlssnr/DlssNr_WindowHost.{h,cpp}`: `RetoqueWindow_Version()` (1), `RetoqueWindow_Begin()` (one session per
  process, owned by the calling thread, refused without `RETOQUE_WINDOW_HOST=1` exactly), `RetoqueWindow_Present(
  swapchain, queue, Outcome*)` and `RetoqueWindow_End()`. Present refuses, before entering anything, from another
  thread, without a swapchain or queue, on an outcome version mismatch, while another present is in the pass, and
  whenever OptiScaler wrapped a swapchain in the process (`State::currentSwapchain`, `currentWrappedSwapchain` or
  `currentFGSwapchain`: that swapchain's Present already runs the pass, and a frame must not be edited twice).
  Otherwise it calls `DlssNr::RunPresentPass(swapchain, queue, false)`, exactly as the wrapped swapchain does, and
  fills the outcome: entered, recorded (the pass count moved), running, the cumulative pass count, and the failure
  reason or the pass's status line. It records no GPU work and owns no GPU object.
- The only change to existing NR code is a read-only accessor, `DlssNr::PassesRecorded()`, beside `IsRunning()` and
  `FailureReason()`. No upstream file changes but the project and filter entries for the new unit.
- **The audit's answers.**
  - `RunPresentPass` takes a raw `IDXGISwapChain3*` and a queue; it needs no wrapper. It reads `State` only to stand
    down for synthesized frame generation and the FG swapchain, both absent in the host. It defers the first four
    presents, reads the swapchain's format and colour space, copies the backbuffer in `PRESENT` state, and submits
    its own lists to the queue it is given. Its resources live until the process exits, as in a game:
    `DlssNr::Shutdown` is never called, and `RetoqueWindow_End` frees nothing (no speculative lifetime change).
  - Loading the DLL as `OptiScaler.dll` runs the `optiscaler.dll` mode of `DllMain`: export detours on `dxgi`, and on
    `d3d12` only with the overlay menu on, among others. The factory vtable hook (`HookFactoryMethods`) runs at the
    exe's entry point, which has already run when the host loads the DLL late. **Decision: the gate does not skip
    hook installation** (that would change `dllmain.cpp`, an upstream file). Instead the host creates its factory,
    both devices, the queue and the swapchain **before** it loads the DLL, never creates another afterwards (a resize
    is `ResizeBuffers`), and its ini turns the overlay menu off. The export's refusal of any wrapped swapchain makes a
    mistake visible instead of a double edit. Whether a detour still interferes with WGC or DComp in practice is for
    the first Windows run to show.
- **Host proof** (`tests/nr-window-host`, host tier): the unit compiled as shipped against fakes, in a process without
  the variable (every export in every order enters nothing) and one with it (exact value, one owner, refusals before
  the pass, an outcome per present, wrapped swapchains refused, no nesting, one session); plus source contracts: the
  exports stay out of `Source.def`, `RunPresentPass`'s signature and its three production callers are unchanged, the
  seam is its only new caller, nothing else in the DLL references the exports or the variable, and the seam calls no
  create, release, submit, wait or shutdown.
- **GPU proof, designed and not built or run.** The loopback mode the plan names would be
  `python3 tests/dlssnr-loopback/run.py --runtime proton --window-host`: a Win32 window and a D3D12 HWND swapchain
  created before `LoadLibrary("OptiScaler.dll")`, the moving scene rendered into the backbuffer, `RETOQUE_WINDOW_HOST=1`
  set by the harness, `RetoqueWindow_Present` before each Present and a read-back after it; `ApplyModel=false`
  byte-identical, `ApplyModel=true` changed with `recorded=1`, the `SynthMotion` oracle, a resize and a clean exit.
  It needs Wine, a GPU and the model, so it was not written blind into the harness here; the first native Windows run
  below covers the same oracle with the real host.

### Phase 2: the capture host, preview output

`retoque/window_host/` (its own `window_host.vcxproj`, in the solution and built by `tools/build-local.sh` after the
forwarder and the DLL; `x64/out/RetoqueWindowHost.exe` is published with them). The [README](../../../retoque/window_host/README.md)
has the command line, the JSON-lines contract and the first-run steps.

- `Geometry.h`, `Policy.h`, `Pacing.h`, `Protocol.h`: pure, Windows-free rules. The client crop (with Magpie's maximized
  case and the no-frame case), the candidate filter (plus display affinity: a window that asks not to be captured is
  refused, never worked around), source identity, run/pause/stop, the adapter rule (the NVIDIA adapter driving the
  source's monitor, else refused), frame pacing (newest wins, strictly later stamps, a new stream after 100 ms, a
  resize, a crop change or a resume), the one-second crop wait, source-age percentiles, and the launch and IPC
  contract (arguments, exit codes, stdout events, stdin commands). Held by `tests/nr-window-host-rules` (host tier,
  g++ with sanitizers, 156 checks).
- `Capture.cpp` (WGC, re-derived from NeuRotic and Magpie, attribution in the file and in
  `Licenses/NRAnything_ATTRIBUTION.txt`), `Transport.cpp` (three shared NT-handle textures and two shared fences:
  D3D11 waits on the GPU until D3D12 read a slot, D3D12 waits until D3D11 wrote it), `Output.cpp` (the preview window
  and its flip-model HWND swapchain), `NrSeam.cpp` (loads `OptiScaler.dll` from the exe's own folder after setting the
  variable, checks the version, begins), `Sources.cpp`, `Devices.cpp`, `Main.cpp`.
- **Where this pipelines where NeuRotic serializes.** The CPU blocks only on the swapchain's frame-latency object.
  A captured surface goes back to WGC once the D3D11 fence shows its copy completed. CPU waits on fences happen only
  before a resize and at exit, bounded at 2 s; a timeout, or a lost device, abandons every object and exits with code 7
  (`restart-required`) rather than release anything the GPU may still use.
- **Not in phase 2:** the overlay, the interaction pause policy (menus, dialogs, move-size), HDR capture (an HDR
  desktop is captured as BGRA8, SDR), any history reset sent to the pass on a new stream (version 1 has no way to ask;
  the trace says when one starts), the app screen, and the measurements (they need Windows).

### First run on native Windows (RTX 3060)

1. Build here: `./build-local.sh Release`. Copy into one new folder on the Windows machine, for example
   `C:\Retoque\NRAnything\` (never a game folder): `x64/out/RetoqueWindowHost.exe`, `x64/out/OptiScaler.dll`,
   `x64/out/nvngx.dll_dlssnr.dll`, and `retoque/window_host/window-host.ini` renamed to `OptiScaler.ini`. Put the
   user's own model in that folder as `nvngx_dlssnr.dll`, or set `[DlssNr] ModelFile=` to its full path.
2. `RetoqueWindowHost.exe --catalog` in a terminal: one `{"event":"window",...}` line per capturable window, then
   `{"event":"catalog",...}`. Pick a windowed, moving source (a video player or an emulator in a window).
3. `RetoqueWindowHost.exe --run --window 0x<hwnd> --max-frames 600 > run1.jsonl` with `ApplyModel=false` in the ini,
   then again with `ApplyModel=true` (`run2.jsonl`). Also `tests/window-capture-probe/artifacts/probe.exe > probe.txt`
   once, for the phase 0 table.
4. **Success looks like:** `run*.jsonl` has `started` with `"nr":true` and the NVIDIA adapter, an `nr` event whose
   `status` names the present route (`present: ...`) with `"running":true`, and `stopped` with
   `"reason":"--max-frames reached"`, `"exit":0`, `presented` near 600, `nr_entered` equal to `presented` and
   `nr_recorded` close to it (the first four presents are deferred by the pass). `OptiScaler.log` beside the exe has
   `DLSS-NR window host: session started (RETOQUE_WINDOW_HOST=1)`, `DLSS-NR present: first pass on the backbuffer
   (WxH, swapchain hook)`, and `DLSS-NR status: present: ...` lines with no `DLSS-NR present:` error. With
   `ApplyModel=false` the preview shows the source unchanged; with it on, the edit is visible. Resize, move to the
   other monitor, minimize (`paused`, then `resumed`) and close the source (`stopped` with "the source window
   closed"). Exit 7 or a `restart-required` event is a failure to report with the log.
5. Record the commit, the three files' SHA-256, the Windows build and driver, and `age_p50_ms`/`age_p95_ms` from
   `stopped`, in a dated evidence record linked from [capabilities](../../capabilities.md).

## Risks

- **Anti-cheat.** Nothing is injected into the game. WGC is the API that OBS and Discord use. But an overlay that
  rewrites a competitive game's image may break a game's rules even where it is not detected, and some anti-cheats
  inspect windows owned by or placed over the game. Keep NeuRotic's warning. Never offer `Route=3`-style injection
  as the fallback.
- **Capture-protected windows.**
  - Apps that call `SetWindowDisplayAffinity` (some launchers, banking or DRM players, games with streamer modes)
    capture as black or not at all. Detect and refuse with a reason, never present black over the source. Whether
    `GetWindowDisplayAffinity` reads another process's window is checked in Phase 0.
  - Exclusive fullscreen cannot have an overlay.
- **DRM video.** Hardware-protected media paths (PlayReady SL3000, Widevine L1 in browsers) capture as black. We must
  not attempt to get around them. The reason is reported, not worked around.
- **Latency.** Capture sees the frame after DWM has composed it, and the overlay is composed later still, so at least
  one refresh interval is added, plus NR time. Input feels later than with an in-process route. Measure it in Phase
  2 and publish it. Do not call this a game mode for fast titles.
- **Cost.** The source renders at full cost, and the NR pass and the copies come on top. On 8 GB cards next to a
  heavy game, VRAM pressure is real. NeuRotic budgets about 1.3 GB of owned memory before the model's own.
- **NGX terms and licensing.** We call the installed driver's NGX core with the same application id that Retoque
  already uses in games, on arbitrary desktop content. Whether NVIDIA's terms allow a capture tool to do so is a
  question for the owner. We never ship `nvngx_dlssnr.dll` or `_nvngx.dll`. The model is the user's file from the
  app catalog. The host folder contains our `OptiScaler.dll` and our forwarder only.
- **Hooks in our own process.** Loading the production DLL installs its hooks in the host. They may interact with
  DComp or WGC. Phase 1 decides whether `RETOQUE_WINDOW_HOST` also skips them.
- **Upstream merges.** The plan touches no upstream file. If the seam turns out to need one (for example wrapping
  composition swapchains, which option B avoids), keep it opt-in and local.
- **Distribution.** A new unsigned exe that captures windows and creates click-through overlays may trip SmartScreen
  or antivirus heuristics.
- **Hybrid and multi-GPU.** A source shown on an iGPU-driven display needs either cross-adapter transport or a
  refusal. NeuRotic refuses; v1 should too.

## Open questions for the owner

Answered on 2026-11-11 except 3 and 5; see [Decisions](#decisions-2026-11-11).

1. **Windows only.** Is a Windows-only feature acceptable, tested on the RTX 3060 box only, with nothing on Linux?
2. **The seam or the code.** Option B makes the host depend on the production DLL through a gated export. Is that
   seam acceptable, or should the NR core be built into the host as a static library? Extracting the renderer is out
   of scope today, per the roadmap.
3. **Recorders.** Should the overlay be hidden from screen capture (`WDA_EXCLUDEFROMCAPTURE`, NeuRotic's choice) so
   that recordings show the original? Or visible, so that streams show NR?
4. **Depth estimation.** Out of scope, as recommended? Or kept as a later proposal: ONNX/DirectML or TensorRT,
   user-built engines, plus a CPU readback.
5. **NGX in a capture tool.** Are you comfortable, under NVIDIA's terms, with NGX evaluating arbitrary desktop
   windows? Should the app show a notice?
6. **Name and placement.** Is "NR Anything" acceptable, or should it be "Any window"? Should it be its own screen in
   the app or part of the game list?
7. **Scope of v1.** Should the first version stay as close to the original as NeuRotic's does (no upscaling, no
   frame generation), with upscaling as the first follow-up?
