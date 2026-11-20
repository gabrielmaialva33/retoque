// Experimental control core; no shipped caller. See README.md for the adapter trust boundary.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Retoque::FrameTransport
{
inline constexpr uint16_t kVersion = 1;
inline constexpr uint32_t kMagic = 0x31544652; // RFT1, little endian
inline constexpr size_t kHeader = 16;
inline constexpr size_t kMaxMessage = 56;
inline constexpr uint32_t kSlots = 2;
inline constexpr uint64_t kInvalidFence = std::numeric_limits<uint64_t>::max();
inline constexpr uint64_t kSharedGpu = 1, kTimeline = 2, kDepth = 4, kMotion = 8;
inline constexpr uint64_t kRequired = kSharedGpu | kTimeline;
inline constexpr uint64_t kKnown = kRequired | kDepth | kMotion;

enum class Refusal
{
    None,
    Truncated,
    Oversized,
    Magic,
    Version,
    Kind,
    Length,
    Reserved,
    Capabilities,
    Capacity,
    NotOpen,
    State,
    Identity,
    Generation,
    Slot,
    Saturated,
    Order,
    Fence,
    Incomplete,
    Busy,
    PeerGone,
    Timeout,
    DeviceLost
};
enum class Kind : uint16_t
{
    Hello = 1,
    Submit = 2,
    Return = 3,
    Release = 4
};
enum class Ownership
{
    Free,
    Submitted,
    Processing,
    Output,
    Abandoned
};

struct Hello
{
    uint64_t offered = 0, required = 0;
    uint32_t slots = 0;
};
struct Agreement
{
    uint64_t capabilities = 0;
    uint32_t slots = 0;
};
inline Refusal Negotiate(const Hello& a, const Hello& b, Agreement& result)
{
    result = {};
    if (a.slots == 0 || b.slots == 0 || a.slots > kSlots || b.slots > kSlots)
        return Refusal::Capacity;
    const auto common = a.offered & b.offered & kKnown;
    const auto needed = a.required | b.required | kRequired;
    if ((common & needed) != needed)
        return Refusal::Capabilities;
    result = { common, std::min(a.slots, b.slots) };
    return Refusal::None;
}

struct Identity
{
    uint64_t session = 0, generation = 0, serial = 0;
    uint32_t slot = 0;
    bool operator==(const Identity&) const = default;
};
struct Frame
{
    Identity id;
    uint64_t fence = 0;
};
struct Message
{
    Kind kind = Kind::Hello;
    Hello hello;
    Frame frame;
};
struct Packet
{
    std::array<uint8_t, kMaxMessage> data {};
    size_t size = 0;
    std::span<const uint8_t> Bytes() const { return { data.data(), std::min(size, data.size()) }; }
};
namespace Detail
{
inline uint64_t Read(std::span<const uint8_t> bytes, size_t offset, size_t count)
{
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value |= uint64_t(bytes[offset + i]) << (8 * i);
    return value;
}
inline void Write(Packet& packet, size_t offset, uint64_t value, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        packet.data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
inline size_t Size(Kind kind) { return kind == Kind::Hello ? 40 : 56; }
inline Packet Header(Kind kind)
{
    Packet packet;
    packet.size = Size(kind);
    Write(packet, 0, kMagic, 4);
    Write(packet, 4, kVersion, 2);
    Write(packet, 6, static_cast<uint16_t>(kind), 2);
    Write(packet, 8, packet.size, 4);
    return packet;
}
} // namespace Detail

inline Packet Encode(const Hello& hello)
{
    auto packet = Detail::Header(Kind::Hello);
    Detail::Write(packet, 16, hello.offered, 8);
    Detail::Write(packet, 24, hello.required, 8);
    Detail::Write(packet, 32, hello.slots, 4);
    return packet;
}
inline Packet Encode(Kind kind, const Frame& frame)
{
    if (kind != Kind::Submit && kind != Kind::Return && kind != Kind::Release)
        return {};
    auto packet = Detail::Header(kind);
    Detail::Write(packet, 16, frame.id.session, 8);
    Detail::Write(packet, 24, frame.id.generation, 8);
    Detail::Write(packet, 32, frame.id.serial, 8);
    Detail::Write(packet, 40, frame.id.slot, 4);
    Detail::Write(packet, 48, frame.fence, 8);
    return packet;
}
// Exactly one framed packet. The eventual byte-stream adapter must accumulate at most kMaxMessage,
// split coalesced packets, and treat EOF/deadline with a partial packet as failure. No allocation here.
inline Refusal Decode(std::span<const uint8_t> bytes, Message& output)
{
    output = {};
    if (bytes.size() < kHeader)
        return Refusal::Truncated;
    const auto length = Detail::Read(bytes, 8, 4);
    if (length > kMaxMessage || bytes.size() > kMaxMessage)
        return Refusal::Oversized;
    if (Detail::Read(bytes, 0, 4) != kMagic)
        return Refusal::Magic;
    if (Detail::Read(bytes, 4, 2) != kVersion)
        return Refusal::Version;
    const auto kind = Detail::Read(bytes, 6, 2);
    if (kind < 1 || kind > 4)
        return Refusal::Kind;
    if (Detail::Read(bytes, 12, 4) != 0)
        return Refusal::Reserved;
    if (bytes.size() < length)
        return Refusal::Truncated;
    if (length != Detail::Size(static_cast<Kind>(kind)) || length != bytes.size())
        return Refusal::Length;
    Message message;
    message.kind = static_cast<Kind>(kind);
    if (message.kind == Kind::Hello)
    {
        if (Detail::Read(bytes, 36, 4) != 0)
            return Refusal::Reserved;
        message.hello = { Detail::Read(bytes, 16, 8), Detail::Read(bytes, 24, 8),
                          static_cast<uint32_t>(Detail::Read(bytes, 32, 4)) };
    }
    else
    {
        if (Detail::Read(bytes, 44, 4) != 0)
            return Refusal::Reserved;
        message.frame = { { Detail::Read(bytes, 16, 8), Detail::Read(bytes, 24, 8), Detail::Read(bytes, 32, 8),
                            static_cast<uint32_t>(Detail::Read(bytes, 40, 4)) },
                          Detail::Read(bytes, 48, 8) };
    }
    output = message;
    return Refusal::None;
}

// One packet at a time: the caller owns any unconsumed suffix, with its own bounded receive queue.
class Reader
{
  public:
    Refusal Feed(std::span<const uint8_t> bytes, size_t& consumed, Message& message, bool& ready)
    {
        consumed = 0;
        ready = false;
        message = {};
        if (failure_ != Refusal::None)
            return failure_;
        while (consumed < bytes.size())
        {
            const size_t count = std::min(target_ - packet_.size, bytes.size() - consumed);
            std::copy_n(bytes.begin() + consumed, count, packet_.data.begin() + packet_.size);
            consumed += count;
            packet_.size += count;
            if (packet_.size < target_)
                break;
            if (target_ == kHeader)
            {
                const auto refusal = Decode(packet_.Bytes(), message);
                if (refusal != Refusal::Truncated)
                    return failure_ = refusal;
                const auto kind = static_cast<Kind>(Detail::Read(packet_.Bytes(), 6, 2));
                target_ = Detail::Read(packet_.Bytes(), 8, 4);
                if (target_ != Detail::Size(kind))
                    return failure_ = Refusal::Length;
            }
            else
            {
                if (const auto refusal = Decode(packet_.Bytes(), message); refusal != Refusal::None)
                    return failure_ = refusal;
                packet_ = {};
                target_ = kHeader;
                ready = true;
                return Refusal::None;
            }
        }
        return Refusal::None;
    }
    Refusal End()
    {
        if (failure_ == Refusal::None && packet_.size != 0)
            failure_ = Refusal::Truncated;
        return failure_;
    }

  private:
    Packet packet_;
    size_t target_ = kHeader;
    Refusal failure_ = Refusal::None;
};

// Single-owner-thread ledger. It owns no GPU object and cannot authenticate a peer or prove completion.
// Observed values MUST come from a trusted local adapter reading the registered fence, not from IPC.
class Session
{
  public:
    Refusal Open(uint64_t session, uint64_t generation, const Hello& local, const Hello& peer)
    {
        if (opened_ || failure_ != Refusal::None)
            return Refusal::State;
        if (session == 0)
            return Refusal::Identity;
        if (generation == 0 || generation == kInvalidFence)
            return Refusal::Generation;
        Agreement agreement;
        const auto refusal = Negotiate(local, peer, agreement);
        if (refusal != Refusal::None)
            return refusal;
        agreement_ = agreement;
        session_ = session;
        generation_ = generation;
        opened_ = true;
        return Refusal::None;
    }

    Refusal Submit(const Frame& frame)
    {
        if (const auto refusal = Check(frame.id); refusal != Refusal::None)
            return refusal;
        if (frame.id.serial <= serial_ || frame.id.serial == kInvalidFence)
            return Refusal::Identity;
        if (Outstanding() >= agreement_.slots)
            return Refusal::Saturated;
        auto& slot = slots_[frame.id.slot];
        if (slot.state != Ownership::Free)
            return Refusal::State;
        if (!NewFence(frame.fence, producerFence_))
            return Refusal::Fence;
        slot = { frame, Ownership::Submitted };
        serial_ = frame.id.serial;
        producerFence_ = frame.fence;
        return Refusal::None;
    }

    Refusal Begin(const Identity& id, uint64_t observedProducer)
    {
        if (const auto refusal = Match(id, Ownership::Submitted); refusal != Refusal::None)
            return refusal;
        for (const auto& slot : slots_)
        {
            if (slot.state == Ownership::Processing)
                return Refusal::State;
            if (slot.state == Ownership::Submitted && slot.frame.id.serial < id.serial)
                return Refusal::Order;
        }
        if (const auto refusal = Completed(observedProducer, slots_[id.slot].frame.fence); refusal != Refusal::None)
            return refusal;
        slots_[id.slot].state = Ownership::Processing;
        return Refusal::None;
    }

    Refusal Return(const Frame& frame, uint64_t observedConsumer)
    {
        if (const auto refusal = Match(frame.id, Ownership::Processing); refusal != Refusal::None)
            return refusal;
        if (!NewFence(frame.fence, consumerFence_))
            return Refusal::Fence;
        if (const auto refusal = Completed(observedConsumer, frame.fence); refusal != Refusal::None)
            return refusal;
        consumerFence_ = frame.fence;
        slots_[frame.id.slot].state = Ownership::Output;
        return Refusal::None;
    }

    Refusal Release(const Frame& frame, uint64_t observedProducer)
    {
        if (const auto refusal = Match(frame.id, Ownership::Output); refusal != Refusal::None)
            return refusal;
        if (!NewFence(frame.fence, producerFence_))
            return Refusal::Fence;
        if (const auto refusal = Completed(observedProducer, frame.fence); refusal != Refusal::None)
            return refusal;
        producerFence_ = frame.fence;
        slots_[frame.id.slot].state = Ownership::Free;
        return Refusal::None;
    }

    Refusal Resize(uint64_t generation)
    {
        if (const auto refusal = Live(); refusal != Refusal::None)
            return refusal;
        if (Outstanding() != 0)
            return Refusal::Busy;
        if (generation <= generation_ || generation == kInvalidFence)
            return Refusal::Generation;
        generation_ = generation;
        slots_ = {};
        serial_ = producerFence_ = consumerFence_ = 0;
        return Refusal::None;
    }

    void Abort(Refusal reason)
    {
        if (failure_ != Refusal::None)
            return;
        failure_ = reason == Refusal::None ? Refusal::State : reason;
        for (auto& slot : slots_)
            if (slot.state != Ownership::Free)
                slot.state = Ownership::Abandoned;
    }
    size_t Outstanding() const
    {
        return std::count_if(slots_.begin(), slots_.end(),
                             [](const auto& slot) { return slot.state != Ownership::Free; });
    }
    Ownership State(uint32_t slot) const { return slot < kSlots ? slots_[slot].state : Ownership::Abandoned; }

  private:
    struct Slot
    {
        Frame frame;
        Ownership state = Ownership::Free;
    };
    Refusal Live() const
    {
        if (failure_ != Refusal::None)
            return failure_;
        return opened_ ? Refusal::None : Refusal::NotOpen;
    }
    Refusal Check(const Identity& id) const
    {
        if (const auto refusal = Live(); refusal != Refusal::None)
            return refusal;
        if (id.session != session_)
            return Refusal::Identity;
        if (id.generation != generation_)
            return Refusal::Generation;
        if (id.slot >= agreement_.slots)
            return Refusal::Slot;
        return Refusal::None;
    }
    Refusal Match(const Identity& id, Ownership state) const
    {
        if (const auto refusal = Check(id); refusal != Refusal::None)
            return refusal;
        const auto& slot = slots_[id.slot];
        if (slot.frame.id != id)
            return Refusal::Identity;
        if (slot.state != state)
            return Refusal::State;
        return Refusal::None;
    }
    static bool NewFence(uint64_t value, uint64_t previous) { return value > previous && value != kInvalidFence; }
    Refusal Completed(uint64_t observed, uint64_t expected)
    {
        if (observed == kInvalidFence)
        {
            Abort(Refusal::DeviceLost);
            return Refusal::DeviceLost;
        }
        return observed >= expected ? Refusal::None : Refusal::Incomplete;
    }
    bool opened_ = false;
    Refusal failure_ = Refusal::None;
    Agreement agreement_;
    uint64_t session_ = 0, generation_ = 0, serial_ = 0, producerFence_ = 0, consumerFence_ = 0;
    std::array<Slot, kSlots> slots_ {};
};
} // namespace Retoque::FrameTransport
