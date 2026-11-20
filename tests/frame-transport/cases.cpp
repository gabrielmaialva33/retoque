#include "pch.h"
#include "Protocol.h"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

using namespace Retoque::FrameTransport;

void Check(bool ok, const char* label)
{
    if (!ok)
    {
        std::cerr << "FAIL: " << label << '\n';
        std::exit(1);
    }
}

Hello Offer() { return { kRequired | kDepth, kRequired, 2 }; }

void Wire()
{
    const auto hello = Encode(Offer());
    Message message;
    Check(Decode(hello.Bytes(), message) == Refusal::None, "hello round trip");
    Check(message.hello.offered == Offer().offered && message.hello.slots == 2, "hello fields");
    auto wrong = hello;
    wrong.data[4] = 2;
    Check(Decode(wrong.Bytes(), message) == Refusal::Version, "wrong version");
    Check(Decode(hello.Bytes().first(hello.size - 1), message) == Refusal::Truncated, "truncated message");
    auto large = hello;
    large.data[8] = 0xff;
    large.data[9] = 0xff;
    Check(Decode(large.Bytes(), message) == Refusal::Oversized, "oversized message");
    wrong = hello;
    wrong.data[12] = 1;
    Check(Decode(wrong.Bytes(), message) == Refusal::Reserved, "reserved header");
    wrong = hello;
    wrong.data[6] = 99;
    Check(Decode(wrong.Bytes(), message) == Refusal::Kind, "unknown kind");
    wrong = hello;
    wrong.data[0] = 0;
    Check(Decode(wrong.Bytes(), message) == Refusal::Magic, "bad magic");
    std::vector<uint8_t> extra(hello.Bytes().begin(), hello.Bytes().end());
    extra.push_back(0);
    Check(Decode(extra, message) == Refusal::Length, "trailing data");
    for (size_t n = 0; n < hello.size; ++n)
        Check(Decode(hello.Bytes().first(n), message) != Refusal::None, "every short prefix");
    Frame frame { { 0x1122334455667788, 3, 0x1234567887654321, 1 }, 42 };
    for (Kind kind : { Kind::Submit, Kind::Return, Kind::Release })
    {
        auto packet = Encode(kind, frame);
        Check(packet.data[16] == 0x88 && packet.data[23] == 0x11, "fixed little endian");
        Check(Decode(packet.Bytes(), message) == Refusal::None && message.kind == kind &&
                  message.frame.id == frame.id && message.frame.fence == 42,
              "frame round trip");
        packet.data[44] = 1;
        Check(Decode(packet.Bytes(), message) == Refusal::Reserved, "reserved frame");
    }
    // Bounded deterministic malformed-input exercise under ASan/UBSan.
    for (unsigned seed = 0; seed < 4096; ++seed)
    {
        Packet packet;
        packet.size = seed % (kMaxMessage + 1);
        uint32_t x = seed + 1;
        for (auto& byte : packet.data)
        {
            x = x * 1664525u + 1013904223u;
            byte = static_cast<uint8_t>(x >> 24);
        }
        (void) Decode(packet.Bytes(), message);
    }
}

void Negotiation()
{
    Agreement agreement;
    Check(Negotiate(Offer(), { kRequired, kRequired, 1 }, agreement) == Refusal::None &&
              agreement.capabilities == kRequired && agreement.slots == 1,
          "capability intersection");
    Check(Negotiate({ kRequired | kDepth, kRequired | kDepth, 2 }, { kRequired, kRequired, 2 }, agreement) ==
              Refusal::Capabilities,
          "missing required capability");
    Check(Negotiate({ kRequired, kRequired, 0 }, Offer(), agreement) == Refusal::Capacity, "zero slots");
    Check(Negotiate({ kRequired, kRequired, 3 }, Offer(), agreement) == Refusal::Capacity, "too many slots");
    Check(Negotiate({ kRequired, kRequired | (1ull << 60), 2 }, Offer(), agreement) == Refusal::Capabilities,
          "unknown required capability");
    Check(Negotiate({ kRequired | (1ull << 60), kRequired, 2 }, Offer(), agreement) == Refusal::None,
          "unknown optional capability ignored");
}

void Framing()
{
    const auto packet = Encode(Offer());
    for (size_t split = 0; split <= packet.size; ++split)
    {
        Reader reader;
        Message message;
        size_t consumed = 99;
        bool ready = false;
        Check(reader.Feed(packet.Bytes().first(split), consumed, message, ready) == Refusal::None &&
                  consumed == split && ready == (split == packet.size),
              "fragment first");
        if (!ready)
            Check(reader.Feed(packet.Bytes().subspan(split), consumed, message, ready) == Refusal::None && ready,
                  "fragment second");
        Check(message.kind == Kind::Hello && message.hello.slots == 2, "fragment content");
        Check(reader.End() == Refusal::None, "clean EOF");
    }
    std::vector<uint8_t> combined(packet.Bytes().begin(), packet.Bytes().end());
    combined.insert(combined.end(), packet.Bytes().begin(), packet.Bytes().end());
    Reader reader;
    Message message;
    size_t consumed = 0;
    bool ready = false;
    Check(reader.Feed(combined, consumed, message, ready) == Refusal::None && ready && consumed == packet.size,
          "coalesced packets stop at one");
    Check(reader.Feed(packet.Bytes().first(9), consumed, message, ready) == Refusal::None && !ready,
          "partial EOF setup");
    Check(reader.End() == Refusal::Truncated, "partial EOF");
    Reader bad;
    auto oversized = packet;
    oversized.data[9] = 0xff;
    Check(bad.Feed(oversized.Bytes().first(16), consumed, message, ready) == Refusal::Oversized && !ready,
          "oversized header refused before payload");
    Check(bad.Feed(packet.Bytes(), consumed, message, ready) == Refusal::Oversized && consumed == 0,
          "framing failure sticky");
}

void Lifecycle()
{
    Session session;
    const Frame a { { 7, 1, 1, 0 }, 1 };
    const Frame b { { 7, 1, 2, 1 }, 2 };
    Check(session.Submit(a) == Refusal::NotOpen, "hello before frames");
    Check(session.Open(7, 1, Offer(), Offer()) == Refusal::None, "open");
    Check(session.Open(7, 1, Offer(), Offer()) == Refusal::State, "no renegotiation");
    Check(session.Submit(a) == Refusal::None && session.Submit(b) == Refusal::None, "two slots");
    Check(session.Submit({ { 7, 1, 3, 0 }, 3 }) == Refusal::Saturated, "queue saturation");
    Check(session.Begin(b.id, 2) == Refusal::Order, "ordered consumption");
    Check(session.Begin(a.id, 0) == Refusal::Incomplete, "capture not complete");
    Check(session.Begin(a.id, 2) == Refusal::None, "begin oldest");
    Check(session.Begin(b.id, 2) == Refusal::State, "one model recording");
    Check(session.Return({ a.id, 1 }, 0) == Refusal::Incomplete, "return not complete");
    auto wrong = a.id;
    wrong.generation = 2;
    Check(session.Return({ wrong, 1 }, 1) == Refusal::Generation, "wrong generation return");
    wrong = a.id;
    wrong.session = 8;
    Check(session.Return({ wrong, 1 }, 1) == Refusal::Identity, "wrong session return");
    wrong = a.id;
    wrong.serial = 9;
    Check(session.Return({ wrong, 1 }, 1) == Refusal::Identity, "wrong serial return");
    Check(session.Return({ a.id, 1 }, 1) == Refusal::None, "return");
    Check(session.Return({ a.id, 2 }, 2) == Refusal::State, "double return");
    Check(session.Resize(2) == Refusal::Busy, "resize busy");
    Check(session.Release({ a.id, 3 }, 2) == Refusal::Incomplete, "copyback not complete");
    Check(session.Release({ a.id, 2 }, 2) == Refusal::Fence, "producer fence cannot rewind");
    Check(session.Release({ a.id, 3 }, 3) == Refusal::None, "release");
    Check(session.Release({ a.id, 4 }, 4) == Refusal::State, "double release");
    Check(session.Submit(a) == Refusal::Identity, "serial replay");
    Check(session.Begin(b.id, 3) == Refusal::None, "begin second");
    Check(session.Return({ b.id, 1 }, 1) == Refusal::Fence, "consumer fence cannot rewind");
    Check(session.Return({ b.id, 2 }, 2) == Refusal::None, "second return");
    Check(session.Release({ b.id, 4 }, 4) == Refusal::None, "second release");
    Check(session.Outstanding() == 0 && session.Resize(2) == Refusal::None, "drained resize");
    Check(session.Resize(2) == Refusal::Generation, "generation replay");
    Check(session.Submit(a) == Refusal::Generation, "old generation submit");
    Check(session.Submit({ { 7, 2, 1, 0 }, 1 }) == Refusal::None, "new generation new fences");
}

void Failure()
{
    Session lost;
    Check(lost.Open(7, 1, Offer(), Offer()) == Refusal::None, "lost open");
    const Frame lostFrame { { 7, 1, 1, 0 }, 1 };
    Check(lost.Submit(lostFrame) == Refusal::None, "lost submit");
    Check(lost.Begin(lostFrame.id, kInvalidFence) == Refusal::DeviceLost, "removed device not complete");
    Check(lost.Begin(lostFrame.id, 1) == Refusal::DeviceLost && lost.State(0) == Ownership::Abandoned,
          "device loss latched");
    for (Refusal reason : { Refusal::PeerGone, Refusal::Timeout, Refusal::DeviceLost })
    {
        for (int phase = 0; phase < 3; ++phase)
        {
            Session session;
            Check(session.Open(7, 1, Offer(), Offer()) == Refusal::None, "failure open");
            Frame frame { { 7, 1, 1, 0 }, 1 };
            Check(session.Submit(frame) == Refusal::None, "failure submit");
            if (phase >= 1)
                Check(session.Begin(frame.id, 1) == Refusal::None, "failure begin");
            if (phase >= 2)
                Check(session.Return(frame, 1) == Refusal::None, "failure return");
            session.Abort(reason);
            Check(session.State(0) == Ownership::Abandoned && session.Outstanding() == 1,
                  "peer gone retains ownership");
            Check(session.Release({ frame.id, 2 }, 2) == reason, "aborted release refused");
            Check(session.Submit({ { 7, 1, 2, 1 }, 3 }) == reason, "aborted submit refused");
            Check(session.Resize(2) == reason, "aborted resize refused");
            Check(session.Open(8, 2, Offer(), Offer()) == Refusal::State, "no restart after uncertainty");
        }
    }
    Session session;
    Check(session.Open(0, 1, Offer(), Offer()) == Refusal::Identity, "zero session");
    Check(session.Open(7, 0, Offer(), Offer()) == Refusal::Generation, "zero generation");
    Check(session.Open(7, 1, Offer(), Offer()) == Refusal::None, "limits open");
    Check(session.Submit({ { 7, 1, 1, 2 }, 1 }) == Refusal::Slot, "slot out of bounds");
    Check(session.Submit({ { 7, 1, 1, 0 }, kInvalidFence }) == Refusal::Fence, "fence overflow");
    Check(session.Submit({ { 7, 1, 0, 0 }, 1 }) == Refusal::Identity, "serial zero");
}

int main()
{
    Wire();
    Negotiation();
    Framing();
    Lifecycle();
    Failure();
    std::cout << "PASS: wire, negotiation, lifecycle, failures\n";
}
