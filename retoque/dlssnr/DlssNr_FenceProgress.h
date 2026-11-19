#pragma once

// What an ID3D12Fence's completed value proves about the work a queue was asked to signal behind.
//
// ID3D12Fence::GetCompletedValue() answers UINT64_MAX once the device has been removed. That is not a value any queue
// reached: the lists behind it were dropped, not run, so it proves nothing about what they were reading. A plain
// "completed >= wanted" still reads it as finished, and "completed < wanted" as nothing left to wait for. The event
// of SetEventOnCompletion is set on a removal as well, so a wait that returned is not completion either: the fence is
// looked at again.
//
// After a removal completion is not proven and never will be, so what those lists used is kept (abandoned), never
// released and never reused: AGENTS.md, "abandon resources after device loss when a safe drain cannot be proved".
//
// Standard library only; tested by tests/nr-present-fence.

#include <cstdint>

namespace DlssNr::FenceProgress
{
enum class State : uint8_t
{
    Completed, // the fence reached the value: every list submitted ahead of that Signal has finished
    Pending,   // not yet; a later look may say otherwise
    Removed,   // the device was removed: nothing is proven, now or later
};

constexpr State Classify(uint64_t completedValue, uint64_t wanted)
{
    if (completedValue == UINT64_MAX)
        return State::Removed;

    return completedValue >= wanted ? State::Completed : State::Pending;
}

// For the places that release or reuse only on proof.
constexpr bool Completed(uint64_t completedValue, uint64_t wanted)
{
    return Classify(completedValue, wanted) == State::Completed;
}
} // namespace DlssNr::FenceProgress
