#include "pch.h"
#include <dlssnr/DlssNr_AdvisorRuntime.h>
#include <cassert>
using namespace DlssNr;
int main()
{
    Advisor::Context context;
    context.device = 1;
    context.width = 1920;
    context.height = 1080;
    assert(AdvisorRuntime::Start({ 1, 1 }, 60));
    uint64_t previous = 0;
    {
        AdvisorRuntime::EvaluationScope eval;
        previous = eval.id;
        AdvisorRuntime::Frame(context, eval.id, true);
    } // a late refusal clears the interval even when no reportSkip ran
    {
        AdvisorRuntime::EvaluationScope eval;
        assert(eval.id == previous + 1 && AdvisorRuntime::Frame(context, eval.id, true).frameMs == 0);
        eval.complete = true;
    }
    AdvisorRuntime::Cancel(Advisor::Abort::Cancelled);
    for (unsigned trigger = 0; trigger < 8; ++trigger)
    {
        assert(AdvisorRuntime::Start({ 1, 1 }, 60));
        auto decision = AdvisorRuntime::Frame(context, 1, true);
        assert(decision.settings && decision.frameMs == 0);
        assert(AdvisorRuntime::Frame(context, 1, true).frameMs == 0);
        auto next = AdvisorRuntime::Frame(context, 2, true);
        assert(next.frameMs > 0 && next.token == decision.token);
        assert(AdvisorRuntime::Frame(context, 4, true).frameMs == 0);
        AdvisorRuntime::SkipEvaluation(4);
        assert(AdvisorRuntime::Frame(context, 5, true).frameMs ==
               0); // missing NR evaluation, independent of generated presents
        if (trigger == 0)
            AdvisorRuntime::Heartbeat(false, true, true);
        if (trigger == 1)
            AdvisorRuntime::Heartbeat(true, false, true);
        if (trigger == 2)
            AdvisorRuntime::Heartbeat(true, true, false);
        if (trigger == 3)
            AdvisorRuntime::Heartbeat(true, true, true, false);
        if (trigger == 4)
            AdvisorRuntime::Heartbeat(true, true, true, true, 256);
        if (trigger == 5)
        {
            auto changed = context;
            ++changed.width;
            AdvisorRuntime::Frame(changed, 3, true);
        }
        if (trigger == 6)
        {
            auto changed = context;
            ++changed.device;
            AdvisorRuntime::Frame(changed, 3, true);
        }
        if (trigger == 7)
            AdvisorRuntime::Cancel(Advisor::Abort::Cancelled);
        assert(!AdvisorRuntime::Active() && !AdvisorRuntime::Snapshot().Override());
        AdvisorRuntime::Confirm({ decision.token, 1, 1, 4, 16, true });
        assert(AdvisorRuntime::Snapshot().Status() == Advisor::State::Aborted);
    }
}
