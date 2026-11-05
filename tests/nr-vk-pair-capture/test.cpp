#include "pch.h"
#include <dlssnr/DlssNr_VkPairCapture.h>
#include <dlssnr/DlssNr_PairCapture.h>
#include <cassert>
#include <iostream>
using namespace DlssNr::VkPairCapture;
struct UntrackedBackend
{
    using Readback = int;
    using Ticket = int;
    using Payload = int;
    using Recording = int;
    int copies = 0, allocations = 0;
    bool Alloc(Recording&, int, Readback& r)
    {
        r = 1;
        ++allocations;
        return true;
    }
    bool Track(Recording&, Ticket&) { return false; }
    void Copy(Recording&, int, Readback&) { ++copies; }
    void Release(Readback& r)
    {
        allocations -= r;
        r = 0;
    }
};
int main()
{
    UntrackedBackend backend;
    DlssNr::PairCapture::Session<UntrackedBackend> session;
    int recording = 0;
    assert(session.Request(0));
    assert(!session.Record(backend, recording, 0, nullptr));
    assert(backend.copies == 0 && backend.allocations == 0);
    auto p = PlanFor(3840, 2160, 97, true, 1ull << 28);
    assert(p && p.dataBytes == 3840ull * 2160 * 16 && p.image.kind == DlssNr::ImageFile::FileKind::Pfm);
    assert(PlanFor(1920, 1080, 37, false, 1ull << 28));
    assert(PlanFor(1920, 1080, 44, false, 1ull << 28));
    assert(PlanFor(2, 2, 109, true, 1024));
    // B10G11R11_UFLOAT_PACK32 (DOOM Eternal's linear DLSS output): PFM when linear HDR, PNG8 when display-referred.
    auto packed = PlanFor(3440, 1440, 122, true, 1ull << 28);
    assert(packed && packed.dataBytes == 3440ull * 1440 * 16 && packed.image.kind == DlssNr::ImageFile::FileKind::Pfm);
    assert(PlanFor(16, 16, 122, false, 1ull << 28).image.kind == DlssNr::ImageFile::FileKind::Png8);
    for (auto format : { 43u, 50u, 64u, 0u })
        assert(!PlanFor(16, 16, format, false, 1ull << 28));
    for (auto format : { 37u, 44u, 64u })
        assert(!PlanFor(16, 16, format, true, 1ull << 28));
    assert(!PlanFor(2, 2, 37, true, 1024));
    assert(!PlanFor(0, 2, 97, true, 1024));
    assert(!PlanFor(~0u, ~0u, 97, true, ~0ull));
    assert(!PlanFor(8192, 8192, 109, true, ~0ull));
    assert(!PlanFor(64, 64, 109, true, 1024));
    Budget budget;
    assert(budget.Reserve(kBudgetBytes));
    assert(!budget.Reserve(1));
    budget.Release(kBudgetBytes);
    assert(budget.Used() == 0);
    assert(!budget.Reserve(kBudgetBytes + 1));

    DlssNr::VkRecording::Registry registry;
    registry.Allocate(1, 2, { 3 });
    registry.Begin(3, DlssNr::VkRecording::Reuse::Serial);
    auto ticket = registry.Current(1, 3);
    assert(ticket && Admissible(*ticket));
    auto state = std::make_shared<Evidence>();
    state->cookie = 17;
    {
        auto pin = std::make_shared<Retirement>(state);
        assert(registry.Pin(*ticket, { pin }));
    }
    int reads = 0;
    uint64_t marker = 0;
    auto inspect = [&]
    {
        return Inspect(*state,
                       [&]
                       {
                           ++reads;
                           return marker;
                       });
    };
    assert(inspect() == Result::Pending && reads == 0);
    marker = 17;
    registry.End(3);
    assert(inspect() == Result::Pending && reads == 0); // even a visible marker is not retirement
    registry.Reset(3);
    assert(inspect() == Result::Readable && reads == 1);
    state->invalid = true;
    assert(inspect() == Result::Discarded && reads == 1);
    state->invalid = false;
    marker = 0;
    assert(inspect() == Result::Discarded); // discarded black is not pixel detection
    marker = 18;
    assert(inspect() == Result::Discarded);
    marker = 17;
    assert(inspect() == Result::Readable);
    state->retired = false;
    state->invalid = true;
    reads = 0;
    assert(inspect() == Result::Discarded && reads == 0); // loss never reads even if Forget releases the pin
    registry.Begin(3, DlssNr::VkRecording::Reuse::Simultaneous);
    assert(!Admissible(*registry.Current(1, 3)));
    registry.Begin(3);
    assert(!Admissible(*registry.Current(1, 3)));
    assert(!Admissible({}));
    std::cout << "PASS: Vulkan format/budget/recording/marker/abandon rules\n";
}
