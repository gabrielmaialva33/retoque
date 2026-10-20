# nr-replay-seam

The DLL side of offline replay (`docs/dlssnr/design/offline-replay-seam.md`), on the host:

- `DlssNr_ReplayClock.h` is the steady clock outside a session, anchored at `Begin` and moved only by the frame's
  logical milliseconds inside one, and the steady clock again after `End`.
- `DlssNr_Replay.cpp`, compiled as it is against fakes for the Windows pieces and a scripted
  `EvaluateAfterUpscale`:
  - refuses without `RETOQUE_REPLAY=1`, a second session, and version mismatches;
  - sets the present counter, the logical clock and the Ray Reconstruction placement;
  - returns one outcome per evaluation with nothing carried over, keeping the first skip reason, truncated.
- Source contract:
  - the renderer's decision clocks (the Dispatch `now`, chain creation spacing, UI-mask and stabilizer gaps, the
    pause gate) read the facade;
  - every skip reason is noted before the once-only log filter;
  - the dispatch summary is noted on the full path;
  - the four exports are unmangled and absent from `Source.def`;
  - the unit is registered once in the project and the filters.

It does not run the production pass, a device or the model; `tests/dlssnr-loopback --replay-nr` does.
