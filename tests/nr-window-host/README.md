# nr-window-host

NR on any window, phase 1 ([design](../../docs/dlssnr/design/nr-anything.md)): the DLL side of the capture host, on
the host.

- `retoque/dlssnr/DlssNr_WindowHost.cpp`, compiled as it ships against fakes for the Windows pieces, `State` and a
  scripted `RunPresentPass`:
  - `inert`: a process without `RETOQUE_WINDOW_HOST`, where every export, in every order, refuses and the pass is
    never entered;
  - `session`: only the exact value `1` opens the one session; one owner thread; refusals (no outcome, outcome
    version, no swapchain or queue, another thread, a swapchain OptiScaler wrapped, a nested present) before the pass;
    the pass entered as `RunPresentPass(swapchain, queue, false)`; one outcome per present (entered, recorded,
    running, count, status or failure, truncated); End ignored from another thread or inside a present; no second
    session.
- Source contract: the four exports unmangled and outside `Source.def`; `RunPresentPass`'s signature and its three
  production callers unchanged and the seam its only new caller; `PassesRecorded()` only reads the counter; nothing
  else in the DLL references the exports or the variable; the seam creates, releases, submits, waits on and shuts down
  nothing; the unit registered once in the project and the filters.

It does not run the pass, a device or the model. The GPU proof is the first native Windows run described in the design
note (a loopback `--window-host` mode is designed there, not built).
