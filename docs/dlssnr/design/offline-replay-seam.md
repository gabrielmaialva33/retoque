# Offline replay: the production seam

2026-10-20. Design note written before the code. It is the DLL side of
[offline-replay.md](offline-replay.md), the contract for capture, run manifests and the judge. That note says what
a replay feeds in and what it must report. This one says how recorded NR-boundary frames reach the production
pass, and what that changes in a normal game: nothing.

## What runs

A replay frame enters `DlssNr::EvaluateAfterUpscale`, the function the game's DLSS evaluate reaches, with a real
`NVSDK_NGX_Parameter` block. The harness allocates the block through the production DLL's own
`NVSDK_NGX_D3D12_AllocateParameters` and fills the keys a game sets:
- Output, Color, Depth, MotionVectors;
- jitter and the motion vector scale;
- Reset;
- DLSS.Pre.Exposure;
- the create flags;
- the render subrect.

From there everything is production code:
- `GatherFrame`, `Dispatch`, chain admission, submission tracking and retirement;
- cadence, the adaptive probes and smoothing;
- composition and the forwarder;
- the user's model.

Nothing of Dispatch is copied into the harness, and the model is never called directly.

What is not covered is the upscaler and everything upstream of it: the game's hooks, NGX feature creation,
Streamline, frame generation, the swapchain. An NR-boundary replay says nothing about those routes.

## The adapter: four exports in a fork-owned unit

`retoque/dlssnr/DlssNr_Replay.cpp` exports, unmangled and outside `Source.def`:

| Export | What it does |
|---|---|
| `uint32_t RetoqueReplay_Version()` | ABI version, 1. |
| `int RetoqueReplay_Begin()` | Starts the process's one session, owned by the calling thread, or refuses with 0: unless the process environment has `RETOQUE_REPLAY=1` (a game never sets it), and always after a session already ran. |
| `int RetoqueReplay_Evaluate(cmd, params, queue, const Frame*, Outcome*)` | One evaluation: sets the logical clock and present counter from the frame, calls `EvaluateAfterUpscale` on `cmd`, fills the outcome. Returns 0, having changed nothing, outside a session, from another thread, when nested inside another evaluation, on a version mismatch, or for a frame whose present or time does not move strictly forward or whose time is not finite and within 10^9 ms. |
| `void RetoqueReplay_End()` | Ends the session (owner thread only); the clock is real again. The pass keeps the history the session built, so no second session follows in the same process. |

`Frame` v1 carries:
- `version`;
- `rayReconstruction` (enters the after-RR placement);
- `present`, which becomes `State::frameCount`, strictly increasing;
- `timeMs`, the logical milliseconds since the session began, strictly increasing.

The caller records `cmd` and executes it. The adapter never submits, waits or reads back, so fence discipline
stays with the harness, as it is with a game.

`Outcome` v1, written only during a session:
- whether Dispatch recorded the pass;
- whether the frame was carried;
- the effective cadence interval (1 for every frame);
- requested and completed model passes;
- the model's result;
- the first skip reason of the evaluation, if any.

Notes are written only by the thread running the evaluation, during it (`thread_local`), so a skip reported from
before the pass takes its lock is still this evaluation's, and nothing another thread does lands in it. A GPU-side
completion or timing claim is not part of it. The harness measures those with its own fences and
timestamps, which keeps "the pass recorded" separate from "the GPU finished it".

## The logical clock

Cadence, adaptive calm, chain and working-size settling, the UI-mask and stabilizer gaps, and the frame-generation
pause all read the steady clock. A harness running faster or slower than a game would otherwise change the
decisions being judged.

`DlssNr_ReplayClock.h` is a facade with one function, `Now()`:
- **No session:** it returns `std::chrono::steady_clock::now()`, the same read as today.
- **During a session:** it returns the instant at `Begin` plus the frame's logical milliseconds.

The decision-making reads move to it:
- the Dispatch `now`, which feeds cadence, the settle gates and chain stability;
- the chain creation spacing, read at each creation as before (a fresh real read outside a session);
- the UI mask and the stabilizer;
- the pause gate;
- the enlargement model's frame time, which is an input to a model.

What the facade does not cover, the offline profile pins: `run_replay.py` writes `HookMethod=1` and
`WhitePointSource=0`, so the present hook and the exposure scan, both live-machine inputs, are out of the replayed
route. A capture whose result depends on them is not supported by v1.

These stay on the real clock:
- GPU fences, timeouts and the GPU timing module;
- log rate limits;
- CPU measurements;
- the pre-upscale route (not in v1).

## Default-off identity

Outside a session, two atomic flag loads per evaluation are added (the clock facade, the outcome note), and no
allocation, no resource and no behaviour. The exports exist in every build; without `RETOQUE_REPLAY=1` they do
nothing. `tests/nr-replay-seam` holds:
- the facade to the real clock when inactive;
- the adapter's refusals;
- the outcome notes to no-ops outside a session.

The host suites that extract Dispatch source learn the facade.

## Validation

- **Host:** the facade and adapter state machine against fakes; the call-site migration under the existing
  suites.
- **Loopback** (`tests/dlssnr-loopback --replay-nr`): replays the moving-scene corpus (offline-replay.md)
  through the production DLL and the real model, in a fresh process per variant, and writes `run.json`.
- **Gates:**
  - NR off leaves every output byte equal to its input;
  - NR on runs the model on every scored frame;
  - Cadence 2 and 4 show their carried frames;
  - adaptive reports its intervals;
  - two identical runs agree on every decision field.

  Pixel agreement between runs is the judge's A/A calibration, not an exact gate.
