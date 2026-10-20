// Offline replay's view of one evaluation (docs/dlssnr/design/offline-replay-seam.md).
//
// The adapter in DlssNr_Replay.cpp hands recorded NR-boundary frames to the production pass. What the pass did with
// each one -- recorded or skipped, carried or run, at which interval, with how many model passes -- is noted here
// while a replay session is active, and nowhere else: outside one every note is a single flag load and a return.
//
// The structs are the adapter's ABI with the loopback harness: plain data, versioned, append-only.

#pragma once

#include <cstdint>

namespace DlssNr::Replay
{
struct Frame
{
    uint32_t version = 1;
    uint32_t rayReconstruction = 0; // enter the after-Ray-Reconstruction placement
    uint64_t present = 0;           // becomes State::frameCount; strictly increasing
    double timeMs = 0.0;            // logical milliseconds since the session began; strictly increasing
};

struct Outcome
{
    uint32_t version = 1;
    uint32_t recorded = 0;        // Dispatch recorded the pass onto the command list
    uint32_t carried = 0;         // model cadence carried the last edit; no model pass ran
    uint32_t interval = 0;        // effective cadence interval; 1 runs the model every frame, 0 when not reached
    uint32_t requestedPasses = 0; // model passes asked for
    uint32_t completedPasses = 0; // model passes recorded successfully; 0 on a carried frame
    int32_t modelResult = 0;      // the model's result for the first pass, 1 on success
    char skip[160] = {};          // the first reason the pass gave for not running, empty when none
};

// From the thread running RetoqueReplay_Evaluate, during that call; no-ops on every other thread and outside a
// replay session. NoteSkip may come from before the pass takes its lock (EvaluateAfterUpscale's early returns).
void NoteSkip(const char* reason);
void NoteDispatch(bool recorded, bool carried, uint32_t interval, uint32_t requested, uint32_t completed,
                  int32_t modelResult);
} // namespace DlssNr::Replay
