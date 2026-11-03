#pragma once

// Zero-motion detection on the Vulkan after-upscale route (docs/dlssnr/design/zero-motion-detection.md, "Vulkan"): what
// the route keeps between evaluates besides the GPU objects. The pass (DlssNr_MotionProbe_Vk) writes a grid and copies
// it into a host-visible buffer; this is the rest.
//
// - Four readback slots, each with a grid and a buffer of its own, each held by the one recording that reserved it
//   until VkRecording sees that recording reset, re-begun or freed (VkMeterReadback::Ring, as the exposure meter's).
//   A recording whose Begin did not rule out a second execution while one is pending is not given a slot
//   (RecordingRefusal): with the slot exclusive to it, nothing else can run over that grid at the same time.
// - A sentinel written into the slot's buffer at reservation, so a recording retired without executing (discarded,
//   or its encode failed) reads as no probe rather than as the grid an earlier recording left there.
// - The probes fed to DlssNr_MotionProbe.h's Detector in the order they were recorded, whatever order the game resets
//   its command buffers in.
// - Whether the verdict may act (P2b of the design review): only with the device its probes were read back on, and
//   only while the probe keeps observing the field. The route drops it (Lose) when the device goes, when the route
//   frees its objects, when the probe cannot observe the frames handed over (no bytecode, the pass not made, the game's
//   views refused, a recording refused), and when no probe has been read back for kStaleEvaluates.
//
// Standard library only: tests/nr-vk-zero-motion drives it with the real Ring and Registry, mapped memory as arrays.
// Not thread-safe; the route calls it under g_vkMutex. Only a recording pin's destruction happens elsewhere.

#include "DlssNr_MotionProbe.h"
#include "DlssNr_VkMeterReadback.h"
#include "DlssNr_VkRecording.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>

namespace DlssNr::VkMotionProbe
{
constexpr uint32_t kSlots = VkMeterReadback::Ring::SlotCount;

// One readback: the grid as FromGrid reads it.
constexpr std::size_t kGridBytes = sizeof(float) * MotionProbe::kGridFloats;

// A verdict whose newest probe was recorded more than this many evaluates ago has stopped observing the field: every
// slot has been held by unreset command buffers for twice the span two compared probes may be apart. It is dropped.
constexpr uint64_t kStaleEvaluates = 2 * MotionProbe::kInterval * MotionProbe::kMaxGapIntervals;

// Why a recording cannot carry a probe, or nullptr. A slot's grid is exclusive to the recording that reserved it, and
// Vulkan does not let a recording begun without SIMULTANEOUS_USE run while it is pending, so the probe's grid has one
// writer at a time. A recording begun with the bit, or whose Begin was not observed with its flags, could run twice at
// once over the same grid, and no barrier inside it would order the two.
constexpr const char* RecordingRefusal(VkRecording::Reuse reuse)
{
    switch (reuse)
    {
    case VkRecording::Reuse::Serial:
        return nullptr;
    case VkRecording::Reuse::Simultaneous:
        return "the game's command buffer was begun with VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT, so two of its "
               "executions could write the probe's grid at once";
    case VkRecording::Reuse::Unknown:
        break;
    }
    return "the flags the game's command buffer was begun with were not observed, so it cannot be shown to run one "
           "execution at a time";
}

class Readbacks
{
  public:
    struct Slot
    {
        uint64_t serial = 0; // the ring's, at reservation
        uint64_t seq = 0;    // the evaluate the probe was recorded at
        MotionProbe::Shape shape {};
    };

    // `mapped` below is the slots' mapped buffers, kSlots of them, each kGridBytes; a slot outside `readyMask` is not
    // used.
    //
    // A free slot among `readyMask` for the probe of evaluate `seq` over `shape`, pinned to `ticket`'s recording, its
    // buffer set to the sentinel. All zero: no cell then carries kMotionTaps^2 samples, so FromGrid reads it invalid
    // and the detector is fed nothing, whatever an earlier probe left in that buffer. nullopt when every ready slot is
    // still held, or the recording refused the pin; nothing is left to undo. Never waits.
    std::optional<VkMeterReadback::Reservation> Reserve(const VkRecording::Ticket& ticket, uint32_t readyMask,
                                                        void* const* mapped, uint64_t seq,
                                                        const MotionProbe::Shape& shape)
    {
        auto reservation = _pending.Reserve(readyMask);
        if (!reservation)
            return std::nullopt;
        try
        {
            if (!VkRecording::Recordings().Pin(ticket, { reservation->pin }))
            {
                _pending.Cancel(*reservation);
                return std::nullopt;
            }
        }
        catch (const std::bad_alloc&)
        {
            _pending.Cancel(*reservation);
            return std::nullopt;
        }

        // The slot is the CPU's alone until the recording is submitted; host writes before a submission are visible to
        // the copy that overwrites them.
        std::memset(mapped[reservation->slot], 0, kGridBytes);
        _slots[reservation->slot] = Slot { reservation->serial, seq, shape };
        return reservation;
    }

    // Every slot whose recording has retired, oldest recording first: read, fed to the detector, and each event handed
    // to say(event, reading). A slot recorded before the newest one already fed (the game reset its buffers out of
    // order) is freed unread. `device` is the device the slots were recorded on; a valid probe gives the verdict that
    // provenance. Returns the slots freed.
    template <class Say> unsigned Consume(uint64_t device, void* const* mapped, uint32_t readyMask, Say&& say)
    {
        VkMeterReadback::Sample retired[kSlots] {};
        unsigned count = 0;
        unsigned freed = 0;
        for (uint32_t slot = 0; slot < kSlots; ++slot)
        {
            const auto sample = _pending.TakeRetired(slot);
            if (!sample)
                continue;
            ++freed;
            // TakeRetired acquires the retirement's publication: the recording is no longer pending and cannot run
            // again. The executed copy's transfer-to-host barrier makes its writes visible in this coherent mapping.
            if (!sample->usable || (readyMask & (uint32_t { 1 } << slot)) == 0 ||
                _slots[slot].serial != sample->serial || sample->serial <= _lastFedSerial)
                continue;
            retired[count++] = *sample;
        }

        std::sort(retired, retired + count, [](const auto& a, const auto& b) { return a.serial < b.serial; });

        for (unsigned i = 0; i < count; ++i)
        {
            const Slot& slot = _slots[retired[i].slot];
            _lastFedSerial = retired[i].serial;
            MotionProbe::FromGrid(static_cast<const float*>(mapped[retired[i].slot]), slot.seq, _probe);
            const MotionProbe::Event event = _detector.Add(slot.shape, _probe);
            if (_probe.valid && _probe.motionSamples != 0)
            {
                ++_fed;
                _device = device;
                _newestSeq = std::max(_newestSeq, slot.seq);
            }
            if (event != MotionProbe::Event::None)
                say(event, _detector.LastReading());
        }

        return freed;
    }

    // Whether the verdict may act on an evaluate on `device`, `evaluates` into the probe's count: it holds, the probes
    // it was formed from were read back on that device, and the newest is no older than kStaleEvaluates.
    bool Authoritative(uint64_t device, uint64_t evaluates) const
    {
        return _detector.Dead() && _fed != 0 && device != 0 && _device == device &&
               evaluates <= _newestSeq + kStaleEvaluates;
    }

    // DlssNr_MotionProbe.h's rule, with the verdict only when it is authoritative. This route always hands the model
    // the game's guides.
    bool PolicyReset(uint64_t device, uint64_t evaluates, bool zeroGuideReset, bool gameReset) const
    {
        return MotionProbe::PolicyReset(Authoritative(device, evaluates), true, zeroGuideReset, gameReset);
    }

    // Whether no valid probe has been read back for more than kStaleEvaluates.
    bool Stale(uint64_t evaluates) const { return _fed != 0 && evaluates > _newestSeq + kStaleEvaluates; }

    // The verdict loses its authority: what the probes saw no longer describes the field being handed over. The
    // detector starts over, and every probe recorded before this boundary is discarded when its recording retires (the
    // slots stay occupied until then), so a verdict forms again only from probes reserved after it. Returns whether it
    // held.
    bool Lose()
    {
        const bool was = _detector.Dead();
        _pending.Invalidate();
        _detector.Reset();
        _fed = 0;
        _device = 0;
        _newestSeq = 0;
        return was;
    }

    // Pending slots keep their occupancy until their recordings retire, and are then freed unread.
    void Invalidate() { _pending.Invalidate(); }

    // The slots' metadata forgotten, no call made: the device is gone, or the caller frees the buffers next. A pin that
    // outlives this retires into state of its own.
    void Abandon() { _pending.Abandon(); }

    const MotionProbe::Detector& Detector() const { return _detector; }
    uint64_t Fed() const { return _fed; }

  private:
    VkMeterReadback::Ring _pending;
    Slot _slots[kSlots] {};
    uint64_t _lastFedSerial = 0;
    MotionProbe::Detector _detector;
    MotionProbe::Probe _probe; // the one being read out
    uint64_t _fed = 0;         // valid probes fed since the verdict last lost its authority
    uint64_t _device = 0;      // the device those were read back on
    uint64_t _newestSeq = 0;   // the newest of them
};
} // namespace DlssNr::VkMotionProbe
