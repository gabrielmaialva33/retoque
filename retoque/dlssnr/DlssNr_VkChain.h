#pragma once

// Portable first slice; NOT wired into the Vulkan renderer. See nr-vk-passes.md before enabling it.
#include "DlssNr_Chain.h"
#include "DlssNr_VkRecording.h"

namespace DlssNr::VkChain
{
enum class Route
{
    AfterUpscale,
    BeforeUpscale,
    Present,
    Proxy,
};

enum class Refusal
{
    None,
    Route,
    CompletionObserver,
    Recording,
    QueueOrder,
    Budget,
};

// Facts the future runtime adapter must prove; none is inferred from elapsed frames or a retired pin.
struct Prerequisites
{
    bool completionObserved = false;
    bool primaryRecording = false;
    bool serialRecording = false;
    bool orderedQueue = false;
    bool budgetAvailable = false;
};

struct Decision
{
    uint32_t count = 0;
    bool legacy = false;
    Refusal refusal = Refusal::None;
};

inline Decision Plan(uint32_t count, Route route, Prerequisites facts)
{
    count = PassConfig::BoundCount(count);
    // Even saved IndividualPassSettings must not alter the existing Vulkan Passes=1 path.
    if (count == 1)
        return { 1, true, Refusal::None };
    if (route != Route::AfterUpscale)
        return { 0, false, Refusal::Route };
    if (!facts.completionObserved)
        return { 0, false, Refusal::CompletionObserver };
    if (!facts.primaryRecording || !facts.serialRecording)
        return { 0, false, Refusal::Recording };
    if (!facts.orderedQueue)
        return { 0, false, Refusal::QueueOrder };
    if (!facts.budgetAvailable)
        return { 0, false, Refusal::Budget };
    return { count, false, Refusal::None };
}

// Evidence for ONE creation recording. Metadata only: Ready never authorizes freeing a resource.
// The adapter owns completion primitives and translates them to a nonzero, non-recycled serial.
// A failed submit, discard, missing observation or device loss must call Invalidate. Replays are
// deliberately refused in v1, including replays after completion; accepting one needs a new design.
class Creation
{
    enum class State
    {
        Invalid,
        Recorded,
        Submitted,
        Complete,
    };
    VkRecording::Ticket _ticket;
    uint64_t _present;
    uint64_t _serial = 0;
    State _state = State::Invalid;

    bool Matches(const VkRecording::Ticket& ticket) const
    {
        return ticket.device == _ticket.device && ticket.buffer == _ticket.buffer && ticket.epoch == _ticket.epoch;
    }

  public:
    Creation(VkRecording::Ticket ticket, uint64_t present) : _ticket(ticket), _present(present)
    {
        if (ticket.device && ticket.buffer && ticket.epoch && present && ticket.reuse == VkRecording::Reuse::Serial &&
            ticket.level == VkRecording::Level::Primary)
            _state = State::Recorded;
    }

    void Invalidate() { _state = State::Invalid; }

    bool Submitted(const VkRecording::Ticket& ticket, uint64_t serial)
    {
        if (!Matches(ticket))
            return false;
        if (_state != State::Recorded || serial == 0)
        {
            Invalidate();
            return false;
        }
        _serial = serial;
        _state = State::Submitted;
        return true;
    }

    bool Complete(const VkRecording::Ticket& ticket, uint64_t serial)
    {
        if (!Matches(ticket) || _state != State::Submitted || serial != _serial)
            return false;
        _state = State::Complete;
        return true;
    }

    bool Ready(uint64_t present) const { return _state == State::Complete && present > _present; }
};

// Same headroom policy as Chain::Admit, subtracting each term to refuse arithmetic overflow too.
// A budget query must describe the heap actually used; aggregate physical memory is not a budget.
inline bool Admit(uint64_t budget, uint64_t used, uint64_t measuredModel, uint64_t textureBytes)
{
    if (!budget || used >= budget)
        return false;
    uint64_t remaining = budget - used;
    const auto consume = [&remaining](uint64_t bytes)
    {
        if (bytes > remaining)
            return false;
        remaining -= bytes;
        return true;
    };
    const uint64_t reserve = std::max<uint64_t>(512ull << 20, budget / 5);
    const uint64_t model = std::max<uint64_t>(measuredModel, 256ull << 20);
    return consume(reserve) && consume(model) && consume(model / 2) && consume(textureBytes);
}

// Each pass owns a latch. A changed tuning can request a replacement, but it does not retire the
// failed feature or prove creation completed. A generation rebuild clears only AFTER safe retirement.
class FailureLatch
{
    std::optional<DlssNrResolvedPassSettings> _failed;

  public:
    void Fail(const DlssNrResolvedPassSettings& settings) { _failed = settings; }
    bool Refused(const DlssNrResolvedPassSettings& settings) const { return _failed && *_failed == settings; }
    void ClearAfterRetirement() { _failed.reset(); }
};
} // namespace DlssNr::VkChain
