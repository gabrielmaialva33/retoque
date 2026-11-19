#pragma once

#include "DlssNr_Advisor.h"
#include <atomic>
#include <chrono>
#include <mutex>

// In-process adapter. No Config mutation, GPU object ownership, or calls into the renderer/timing collector.
namespace DlssNr::AdvisorRuntime
{
inline std::atomic<bool> active { false }, resultReady { false };
inline std::mutex mutex;
inline Advisor::Session session;
inline uint64_t previousEvaluation = 0, previousToken = 0;
inline double previousTime = 0;
inline std::atomic<uint64_t> evaluations { 0 };
// Count every attempted Dispatch while active, including early refusals: the next interval then has a gap.
inline uint64_t BeginEvaluation()
{
    return active.load(std::memory_order_relaxed) ? evaluations.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
}
inline double Now()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline bool Active() { return active.load(std::memory_order_relaxed); }
inline void Publish()
{
    session.Restored();
    active.store(session.Active(), std::memory_order_relaxed);
    resultReady.store(session.Status() == Advisor::State::Done && session.ResultValid(), std::memory_order_relaxed);
}
inline void Cancel(Advisor::Abort why)
{
    if (!Active() && !resultReady.load(std::memory_order_relaxed))
        return;
    std::lock_guard lock(mutex);
    session.InvalidateResult();
    session.Cancel(why);
    Publish();
}
inline bool Start(Advisor::Settings original, double target)
{
    std::lock_guard lock(mutex);
    const bool started = session.Start(original, target, Now());
    if (started)
    {
        previousEvaluation = previousToken = 0;
        previousTime = 0;
    }
    Publish();
    return started;
}
inline Advisor::Session Snapshot()
{
    std::lock_guard lock(mutex);
    return session;
}
inline void Heartbeat(bool visible, bool enabled, bool timing, bool supported = true, uint32_t hostRoute = 0)
{
    if (!Active())
        return;
    std::lock_guard lock(mutex);
    if (!visible)
        session.Cancel(Advisor::Abort::MenuClosed);
    else if (!enabled)
        session.Cancel(Advisor::Abort::Disabled);
    else if (!timing)
        session.Cancel(Advisor::Abort::TimingOff);
    else if (!supported)
        session.Cancel(Advisor::Abort::Unsupported);
    else if (session.BoundContext() && (session.BoundContext()->route & ~1u) != hostRoute)
        session.Cancel(Advisor::Abort::RouteChanged);
    else
        session.Tick(Now());
    Publish();
}
struct Decision
{
    std::optional<Advisor::Settings> settings;
    uint64_t token = 0;
    double frameMs = 0;
};
inline Decision Frame(const Advisor::Context& context, uint64_t evaluation, bool supported)
{
    if (!Active())
        return {};
    std::lock_guard lock(mutex);
    const double now = Now();
    if (!supported)
        session.Cancel(Advisor::Abort::Unsupported);
    session.Observe(context);
    session.Tick(now);
    Publish();
    Decision result { session.Override(), session.Token(), 0 };
    if (result.token && result.token == previousToken && evaluation == previousEvaluation + 1 && previousEvaluation)
        result.frameMs = now - previousTime;
    if (evaluation != previousEvaluation || result.token != previousToken)
    {
        previousTime = now;
        previousEvaluation = evaluation;
        previousToken = result.token;
    }
    return result;
}
inline void SkipEvaluation(uint64_t evaluation)
{
    if (!Active())
        return;
    std::lock_guard lock(mutex);
    if (evaluation == previousEvaluation)
        previousEvaluation = 0;
}
struct EvaluationScope
{
    uint64_t id = BeginEvaluation();
    bool complete = false;
    ~EvaluationScope()
    {
        if (id && !complete)
            SkipEvaluation(id);
    }
};
inline void Refuse(const char* reason, uint64_t token)
{
    if (!Active())
        return;
    std::lock_guard lock(mutex);
    if (token && session.Token() == token)
        session.Refuse(reason ? reason : "");
}
inline void Confirm(const Advisor::Sample& sample)
{
    if (!Active())
        return;
    std::lock_guard lock(mutex);
    session.Accept(sample, Now());
    Publish();
}
} // namespace DlssNr::AdvisorRuntime
