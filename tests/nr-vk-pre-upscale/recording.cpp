#include "dlssnr/DlssNr_VkPreObserve.h"
#include <cassert>
#include <cstdio>
#include <type_traits>

using namespace DlssNr;
using namespace DlssNr::VkPre;
static_assert(std::is_same_v<Ticket, VkRecording::Ticket>);
int main()
{
    Observations o;
    VkRecording::Registry r;
    o.Device(1);
    o.Pool(1, 10, 4);
    o.Allocate(1, 10, 20, true);
    r.Allocate(1, 10, { 20 });
    auto begin = [&]
    {
        auto prior = r.Current(1, 20);
        r.Begin(20, VkRecording::Reuse::Serial);
        auto now = r.Current(1, 20);
        o.Begin(20, true, true, false, prior, now);
        return now;
    };
    auto first = begin();
    assert(first && o.Associated(1, 20, first));
    assert(o.Command(20)->ticket->epoch == first->epoch);
    o.Begin(20, false, true, false, first, {});
    assert(o.Associated(1, 20, first)); // a failed Begin must not retire the old recording
    auto next = begin();
    assert(next->epoch != first->epoch && o.Associated(1, 20, next));
    assert(!o.Associated(1, 20, first));
    o.Begin(20, true, true, false, next, next); // lifecycle hook missed this Begin
    assert(!o.Associated(1, 20, next));
    o.Begin(20, true, true, false, {}, {}); // lifecycle hook runs after us
    r.Begin(20, VkRecording::Reuse::Serial);
    auto late = r.Current(1, 20);
    assert(!o.Associated(1, 20, late)); // cannot repair by attaching a later Current
    auto checkBad = [&](Ticket bad)
    {
        o.Begin(20, true, true, false, {}, bad);
        assert(!o.Associated(1, 20, bad));
    };
    checkBad({ 2, 20, 99, VkRecording::Reuse::Serial });
    checkBad({ 1, 21, 99, VkRecording::Reuse::Serial });
    checkBad({ 1, 20, 0, VkRecording::Reuse::Serial });
    checkBad({ 1, 20, 99, VkRecording::Reuse::Unknown });
    checkBad({ 1, 20, 99, VkRecording::Reuse::Simultaneous });
    next = begin();
    auto wrongReuse = *next;
    wrongReuse.reuse = VkRecording::Reuse::Unknown;
    assert(!o.Associated(1, 20, wrongReuse));
    r.Begin(20, VkRecording::Reuse::Serial); // unobserved later recording
    assert(!o.Associated(1, 20, r.Current(1, 20)));
    next = begin();
    o.End(20, true);
    assert(!o.Associated(1, 20, next));
    next = begin();
    o.ResetBuffer(20, true);
    assert(!o.Associated(1, 20, next));
    next = begin();
    o.ResetPool(1, 10, true);
    assert(!o.Associated(1, 20, next));
    next = begin();
    o.Free(1, 20);
    assert(!o.Associated(1, 20, next));
    o.Allocate(1, 10, 20, true);
    assert(!o.Associated(1, 20, next));
    next = begin();
    o.DestroyDevice(1);
    assert(!o.Associated(1, 20, next));
    std::puts("PASS: canonical recording identity; missed, stale and misordered lifecycle evidence refused");
}
