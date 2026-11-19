# NR present sync

```bash
python3 tests/nr-present-sync/run.py
```

The runner compiles `retoque/dlssnr/DlssNr_PresentSync.h` with g++ C++20, ASan/UBSan, and `-Werror`.

## What this covers

- **Auto without frame generation** does not wait: the write-back and the flip are on one queue.
- **Auto with any sign of frame generation** waits: called from a frame generation present hook, OptiScaler's own FG
  output selected, DLSS-G options or frame counts seen, or Streamline's DLSS-G plugin / `nvngx_dlssg.dll` loaded.
- **Auto at the game's own Present**, ahead of its DLSS-G, does not wait.
- **Auto under FSR-FG's own swapchain at the frame generation hook** does not wait (2026-11-18): the output is FSR-FG
  (FFX's swapchain or the fork's presenter, LSFG included), no custom provider is named, and the pass was submitted on
  the queue the swapchain was created on. Each condition is needed: another output (XeFG, DLSS-G, the flip meter,
  reprojection), a custom provider, another queue or the wrapped entry waits, and so does DLSS-G seen or loaded.
- **Multi-pass recordings admitted one at a time** keep the wait under FSR-FG: the next frame's recording would be
  refused while this one is unfinished (chain-in-flight.md). The input changes no other answer.
- **An explicit `true` or `false`** wins over all of that.
- **The reason** that goes with each answer, one literal per branch, which the log prints when it changes.
- **Mutation controls**: seven changes to the header, each taking one condition of the FSR-FG exception out (or
  moving it ahead of DLSS-G), each fail a case.
- **Source guards on what the exception rests on**: `RunPresentPass` fills the FSR-FG inputs and decides once, after
  the submit; `Submission::Serialized` is the admission rule's own two conditions (strict mode, or no overlap);
  `FGHooks::FGPresent` runs the pass on the FG object's game queue before the swapchain's `Present`;
  `FSRFG_Dx12` creates the swapchain on the queue it keeps; and the fork's presenter signals a fence on its game queue
  and makes the queue that reads the frame wait for it, in both present paths. FFX's vendored swapchain is held to
  the same text when `external/FidelityFX-SDK-v2` is checked out, and the runner says when it is not.

## What this does not cover

The inputs are gathered in `RunPresentPass` (`DlssNr_Dx12.cpp`) from `State` and the loader; that code and the wait
itself are not exercised here. Whether the write-back can race a paced flip is the design note's question
(`docs/dlssnr/design/nr-present-hook.md`), not this suite's: the guards hold the source text the note read, not a
GPU's behaviour, and not the FFX DLL a game folder actually loads.
