#include "pch.h"
#include <dlssnr/DlssNr_Advisor.h>
#include <cassert>
#include <limits>
using namespace DlssNr::Advisor;
int main()
{
    auto choices = Candidates({ 1, 1 });
    assert(choices.size() == 6 && choices[1].scale == .75f && choices[4].cadence == 2);
    Context context;
    context.device = 1;
    context.width = 1920;
    context.height = 1080;
    Session session;
    assert(!session.Start({ NAN, 1 }, 60, 0));
    assert(session.Start({ 1, 1 }, 60, 0));
    session.Observe(context);
    const auto first = session.Token();
    Sample s { first, 1, 1, 4, 16, true, false, false };
    session.Accept(s, 1000);
    session.Accept(s, 1000);
    assert(session.Count() == 1);
    s.id = 2;
    s.token = first + 1;
    session.Accept(s, 1000);
    assert(session.Count() == 1);
    s.token = first;
    s.gpuMs = NAN;
    session.Accept(s, 1000);
    assert(session.Count() == 1);
    s.gpuMs = 4;
    s.frameMs = 0;
    session.Accept(s, 1000);
    assert(session.Count() == 1);
    s.frameMs = 16;
    s.confirmed = false;
    session.Accept(s, 1000);
    assert(session.Count() == 1);
    s.confirmed = true;
    s.reset = true;
    session.Accept(s, 1000);
    assert(session.Count() == 1);
    s.reset = false;
    for (uint64_t i = 2; i <= 30; ++i)
    {
        s.id = i;
        session.Accept(s, 1000);
    }
    assert(session.Status() == State::Sampling && session.Count() == 0);
    for (uint64_t i = 31; i <= 90; ++i)
    {
        s.id = i;
        session.Accept(s, 1100);
    }
    assert(session.Index() == 1 && session.Results().size() == 1 && session.Token() != first);
    assert(session.Results()[0].medianMs == 4 && session.Results()[0].share == 25);
    session.Refuse("runtime refusal");
    session.Tick(21101);
    assert(session.Results()[1].reason == "runtime refusal");
    while (session.Active())
        session.Tick(100000.0 * (session.Index() + 1));
    assert(!session.Override() && session.Status() == State::Restoring);
    session.Restored();
    assert(session.Status() == State::Done && session.Recommendation() == 0);
    assert(session.Start({ 1, 1 }, 60, 0) && session.Token() > first);
    for (auto why : { Abort::Cancelled, Abort::MenuClosed, Abort::Disabled, Abort::TimingOff, Abort::RouteChanged,
                      Abort::Resized, Abort::DeviceLost, Abort::Unsupported })
    {
        if (!session.Active())
            assert(session.Start({ 1, 1 }, 60, 0));
        session.Cancel(why);
        assert(!session.Override());
        session.Restored();
        assert(session.Status() == State::Aborted && session.AbortReason() == why && !session.Recommendation());
    }
    assert(session.Start({ 1, 1 }, 60, 0));
    session.Observe(context);
    auto changed = context;
    changed.width++;
    session.Observe(changed);
    session.Restored();
    assert(session.AbortReason() == Abort::Resized);
    for (auto why : { Abort::DeviceLost, Abort::RouteChanged, Abort::Unsupported })
    {
        assert(session.Start({ 1, 1 }, 60, 0));
        session.Observe(context);
        changed = context;
        if (why == Abort::DeviceLost)
            ++changed.device;
        if (why == Abort::RouteChanged)
            ++changed.route;
        if (why == Abort::Unsupported)
            changed.original.scale = .5f;
        session.Observe(changed);
        session.Restored();
        assert(session.Status() == State::Aborted && session.AbortReason() == why && !session.Override());
    }
    assert(session.Start({ 1, 1 }, 60, 0));
    session.Observe(context);
    // Minimum time AND count; subsequent contract change aborts the sampling window.
    for (uint64_t i = 1; i <= 30; ++i)
        session.Accept({ session.Token(), i, 1, 4, 16, true }, 1);
    assert(session.Status() == State::Settling);
    session.Accept({ session.Token(), 31, 1, 4, 16, true }, 1000);
    assert(session.Status() == State::Sampling);
    session.Accept({ session.Token(), 32, 2, 4, 16, true }, 1100);
    session.Restored();
    assert(session.Status() == State::Aborted);
    assert(session.Start({ 1, 1 }, 60, 0));
    session.Observe(context);
    session.Accept({ session.Token(), 1, 1, 4, 16, true }, 1000);
    session.Accept({ session.Token(), 2, 2, 4, 16, true }, 19999);
    session.Tick(20000); // contract churn must not postpone the absolute timeout
    assert(session.Index() == 1 && !session.Results()[0].reason.empty());
    session.Cancel(Abort::Cancelled);
    session.Restored();
    assert(session.Start({ 1, 1 }, 60, 0));
    session.Observe(context);
    for (uint64_t i = 100; i < 130; ++i)
        session.Accept({ session.Token(), i, 1, 4, 16, true }, 1000);
    assert(session.Status() == State::Sampling);
    session.Accept({ session.Token(), 50, 1, 4, 16, true }, 1100); // unseen but older than settling watermark
    assert(session.Count() == 0);
    session.Accept({ session.Token(), 130, 1, 4, 16, true }, 1100);
    assert(session.Count() == 1);
    session.Cancel(Abort::Cancelled);
    session.Restored();
    std::vector<Sample> table;
    for (uint64_t i = 1; i <= 20; ++i)
        table.push_back({ 1, i, 1, double(i), 16, true });
    auto stats = Summarize({ 1, 1 }, table);
    assert(stats.medianMs == 10.5 && stats.worst5Ms == 20);
    const auto share =
        Summarize({ 1, 1 }, { { 1, 1, 1, 1, 1, true }, { 1, 2, 1, 3, 100, true }, { 1, 3, 1, 9, 2, true } });
    assert(share.share == 150); // ratio of window medians, not a misleading per-frame CPU/GPU pairing
    std::vector<Result> results { stats, stats };
    results[0].count = results[1].count = 60;
    results[0].frameMs = 20;
    results[1].frameMs = 15;
    assert(Recommend(results, 60) == 1 && !Recommend(results, 100) && !Recommend(results, NAN));
    results[1].count = 59;
    assert(!Recommend(results, 60));
    results[1].count = 60;
    results[1].reason = "refused";
    assert(!Recommend(results, 60));
}
