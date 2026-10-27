#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace DlssNr::VkMeterReadback
{
struct Reservation
{
    uint32_t slot = 0;
    uint64_t serial = 0;
    std::shared_ptr<void> pin;
};

struct Sample
{
    uint32_t slot = 0;
    uint64_t serial = 0;
    bool usable = false;
};

// Ring operations require the owner's existing serialization. Only destruction of
// the last recording pin may occur on another thread. Retirement means the owner
// has retired the recording under its existing GPU-safety boundary; this helper
// neither observes submission completion nor issues any Vulkan calls.
class Ring
{
    struct RetirementState
    {
        std::atomic<bool> retired { false };
    };

    struct RetirementToken
    {
        std::shared_ptr<RetirementState> state;

        // Construct directly in make_shared: a temporary token's destructor would
        // announce retirement before the actual recording releases its pin.
        explicit RetirementToken(std::shared_ptr<RetirementState> value) noexcept : state(std::move(value)) {}
        RetirementToken(const RetirementToken&) = delete;
        RetirementToken& operator=(const RetirementToken&) = delete;
        ~RetirementToken() { state->retired.store(true, std::memory_order_release); }
    };

    struct Entry
    {
        std::shared_ptr<RetirementState> state;
        std::weak_ptr<void> token;
        uint64_t serial = 0;
        bool usable = false;
    };

  public:
    static constexpr uint32_t SlotCount = 4;

  private:
    std::array<Entry, SlotCount> _entries {};
    uint64_t _nextSerial;

  public:
    // Zero denotes permanent serial exhaustion. The last nonzero serial can be
    // issued once; Abandon never resets this object's serial high-water mark.
    explicit Ring(uint64_t initialSerial = 1) noexcept : _nextSerial(initialSerial) {}
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    std::optional<Reservation> Reserve(uint32_t readyMask) noexcept
    {
        if (_nextSerial == 0)
            return std::nullopt;
        for (uint32_t slot = 0; slot < SlotCount; ++slot)
        {
            if ((readyMask & (uint32_t { 1 } << slot)) == 0 || _entries[slot].state)
                continue;

            // Allocate everything before publishing occupancy or advancing the
            // serial. The ring holds state and a weak identity, never the token.
            std::shared_ptr<RetirementState> state;
            std::shared_ptr<void> token;
            try
            {
                state = std::make_shared<RetirementState>();
                token = std::make_shared<RetirementToken>(state);
            }
            catch (const std::bad_alloc&)
            {
                return std::nullopt;
            }

            const uint64_t serial = _nextSerial;
            _entries[slot] = Entry { std::move(state), token, serial, true };
            _nextSerial = serial == std::numeric_limits<uint64_t>::max() ? 0 : serial + 1;
            return Reservation { slot, serial, std::move(token) };
        }
        return std::nullopt;
    }

    // Only for admission/pinning failure before any command references the slot.
    // A stale/foreign reservation cannot cancel a newer occupant of the same slot.
    void Cancel(const Reservation& reservation) noexcept
    {
        if (reservation.slot >= SlotCount || !reservation.pin)
            return;
        auto& entry = _entries[reservation.slot];
        if (entry.state && entry.serial == reservation.serial && !entry.token.owner_before(reservation.pin) &&
            !reservation.pin.owner_before(entry.token))
            entry = Entry {};
    }

    std::optional<Sample> TakeRetired(uint32_t slot) noexcept
    {
        if (slot >= SlotCount)
            return std::nullopt;
        auto& entry = _entries[slot];
        if (!entry.state || !entry.state->retired.load(std::memory_order_acquire))
            return std::nullopt;
        const Sample sample { slot, entry.serial, entry.usable };
        entry = Entry {};
        return sample;
    }

    // Keep every pending slot occupied until its pin retires, but discard its
    // eventual value (for example after a temporal/exposure reset).
    void Invalidate() noexcept
    {
        for (auto& entry : _entries)
            if (entry.state)
                entry.usable = false;
    }

    // Device abandonment forgets metadata only. Surviving tokens own independent
    // state; their later destruction cannot access the ring or any GPU resource.
    void Abandon() noexcept
    {
        for (auto& entry : _entries)
            entry = Entry {};
    }
};
} // namespace DlssNr::VkMeterReadback
