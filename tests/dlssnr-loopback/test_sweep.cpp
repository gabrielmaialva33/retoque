#include "sweep_safety.h"
#include <cassert>
#include <cstdint>
#include <limits>

int main()
{
    SweepSafety::Coverage c;
    assert(!c.end_step());
    c.begin_step();
    assert(!c.evaluation(false));
    assert(!c.end_step());
    c.begin_step();
    assert(c.evaluation(true));
    assert(!c.evaluation(false));
    assert(!c.end_step());
    c.begin_step();
    assert(c.evaluation(true));
    assert(c.end_step());
    for (int fault = 0; fault != 6; ++fault)
    {
        int stage = 0;
        auto signal = [&]
        {
            ++stage;
            return fault != 1;
        };
        auto event = [&]
        {
            ++stage;
            return fault != 2;
        };
        auto wait = [&]
        {
            ++stage;
            return fault == 3 ? 258u : fault == 4 ? 0xffffffffu : 0u;
        };
        auto value = [&]
        {
            ++stage;
            return fault == 5 ? UINT64_MAX : uint64_t { 7 };
        };
        bool ok = SweepSafety::drain(signal, event, wait, value, uint64_t { 7 });
        assert(ok == (fault == 0));
        assert(stage == (fault == 1 ? 1 : fault == 2 ? 2 : fault == 3 || fault == 4 ? 3 : 4));
    }
    assert(SweepSafety::completed(true, true, true, 7, 7));
    assert(!SweepSafety::completed(false, true, true, 7, 7));
    assert(!SweepSafety::completed(true, false, true, 7, 7));
    assert(!SweepSafety::completed(true, true, false, 7, 7));
    assert(!SweepSafety::completed(true, true, true, 6, 7));
    assert(!SweepSafety::completed(true, true, true, std::numeric_limits<uint64_t>::max(), 7));
}
