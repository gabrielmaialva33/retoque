#pragma once

#include "DlssNr_SubmissionModel.h"
#include "DlssNr_GpuTimingModel.h"
#include <d3d12.h>
#include <memory>

namespace DlssNr::Submission
{
using Usage = Detail::Usage;

// Opt-in only. Call before recording ANY use of the retained object/model. False
// refuses the work. Keep one Usage per resource generation and copy it at retirement.
// chain: an NR chain recording (docs/dlssnr/design/chain-in-flight.md), the only kind that sets queue affinity.
bool Track(ID3D12GraphicsCommandList* list, Usage& usage, bool chain = false);
bool Ready(const Usage& usage);

// docs/dlssnr/design/chain-in-flight.md. The renderer's evaluation count, advanced on every Dispatch entry.
void Advance(uint64_t now);
uint64_t Clock();
// A list executed again without a Reset was seen: admission and retirement are back to Ready.
bool Strict();
// Tracked work was submitted on more than one queue: overlap is refused for the session.
bool MultiQueue();
// May a new tracked recording into list start? Prunes the lifetime set first: Ready and cancelled epochs, and
// completed ones once aged out of the replay window.
Detail::Admission Admit(Usage& usage, ID3D12GraphicsCommandList* list);
// Every epoch finished -- Ready, cancelled, or (outside strict mode) completed: freeing is allowed.
bool Settled(const Usage& usage);
// Not settled only because of aged epochs no fence will answer: retained, but out of the parking cap.
bool OnlyUnanswerableLeft(const Usage& usage);

// Exact recording, single execution and actual queue frequency; also reports safe slot reuse.
GpuTiming::Detail::Certificate TimingCertificate(const Usage& usage);
void Prune(Usage& usage);

// Completion observed so far, NOT permission to destroy: a list may execute again.
bool Completed(const Usage& usage);
bool SubmittedOnlyOn(const Usage& usage, ID3D12CommandQueue* queue);
void Abandon(ID3D12GraphicsCommandList* list);
bool Active();
const char* LastRefusal();

// Hook boundary: capture exact epochs BEFORE the original ExecuteCommandLists,
// then Signal AFTER it returns. The batch serializes tracked queue submissions so
// monotonically increasing fence values cannot be signaled out of order.
class Batch
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    Batch(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    ~Batch();
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    void Submitted();
};
} // namespace DlssNr::Submission
