#pragma once

#include <atomic>
#include <cstdint>

// Camera cuts belong to the model history, not to a guide-ring slot. In particular, a failed capture must not
// discard a cut. Idea: MFGAdaUnlock-RenoDx 1912c33 (N1); independent implementation, no resource handling.
namespace DlssNr::PresentReset
{
class Pending
{
  public:
    void Observe(bool enabled, bool reset)
    {
        if (enabled && reset)
            _issued.fetch_add(1, std::memory_order_relaxed);
    }

    // Zero means no request. A snapshot is not a take: skips and abandoned recordings leave it pending.
    uint64_t Snapshot() const
    {
        const auto consumed = _consumed.load(std::memory_order_relaxed);
        const auto issued = _issued.load(std::memory_order_relaxed);
        return issued > consumed ? issued : 0;
    }

    template <typename Frame> static void Apply(uint64_t ticket, Frame& frame)
    {
        if (ticket == 0)
            return;
        frame.Reset = true;
        frame.ResetIsPolicy = false;
        frame.ResetIsSceneCut = false;
    }

    // Confirm only the snapshot carried by a model recording that was actually submitted. A concurrent cut stays
    // pending, and confirmation of an older recording cannot move consumption backwards. No GPU lifetime changes.
    void Confirm(uint64_t ticket, bool recorded, bool submitted)
    {
        if (ticket == 0 || !recorded || !submitted)
            return;
        auto consumed = _consumed.load(std::memory_order_relaxed);
        while (consumed < ticket && !_consumed.compare_exchange_weak(consumed, ticket, std::memory_order_relaxed))
        {
        }
    }

  private:
    std::atomic<uint64_t> _issued { 0 };
    std::atomic<uint64_t> _consumed { 0 };
};

inline Pending& Instance()
{
    static Pending pending;
    return pending;
}
} // namespace DlssNr::PresentReset
