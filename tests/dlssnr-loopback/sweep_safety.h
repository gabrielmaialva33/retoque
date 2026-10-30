#pragma once
#include <cstdint>
#include <limits>

namespace SweepSafety
{
struct Coverage
{
    unsigned attempts = 0, successes = 0;
    bool failed = false;
    void begin_step()
    {
        attempts = successes = 0;
        failed = false;
    }
    bool evaluation(bool ok)
    {
        ++attempts;
        if (ok)
            ++successes;
        else
            failed = true;
        return ok;
    }
    bool end_step() const { return !failed && attempts > 0 && successes == attempts; }
};
inline bool completed(bool signal, bool event, bool wait, uint64_t observed, uint64_t target)
{
    return signal && event && wait && observed != std::numeric_limits<uint64_t>::max() && observed >= target;
}
template <class Signal, class Event, class Wait, class Value>
bool drain(Signal signal, Event event, Wait wait, Value value, uint64_t target)
{
    if (!signal() || !event() || wait() != 0)
        return false;
    return completed(true, true, true, value(), target);
}
} // namespace SweepSafety
