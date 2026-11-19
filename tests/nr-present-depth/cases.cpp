#include "pch.h"
#include "retoque/dlssnr/DlssNr_PresentDepth.h"
#include "retoque/dlssnr/DlssNr_PresentDepthTables.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <source_location>
using namespace DlssNr::PresentDepth;
namespace
{
int checks = 0;
void Check(bool value, std::source_location location = std::source_location::current())
{
    ++checks;
    if (!value)
    {
        std::cerr << "line " << location.line() << ": failed check " << checks << '\n';
        std::abort();
    }
}
Shape ShapeFor(unsigned w = 1920, unsigned h = 1080, unsigned samples = 1)
{
    Shape s;
    s.width = w;
    s.height = h;
    s.samples = samples;
    s.format = 39;
    s.placement = Placement::Committed;
    return s;
}
Id Make(Model& m, Shape shape = ShapeFor(), bool life = true, bool state = true)
{
    return m.Create(shape, life, state, 16);
}
void EventFor(Model& m, Id id, Kind kind, uint64_t count = 1, float depth = 0, bool complete = true, uint64_t queue = 1)
{
    Event e;
    e.id = id;
    e.kind = kind;
    e.count = count;
    e.depth = depth;
    e.complete = complete;
    e.viewFormat = 40;
    e.timeNs = 100;
    m.Apply(e, queue);
}
void Scene(Model& m, Id id, uint64_t draws = 10, uint64_t queue = 1)
{
    EventFor(m, id, Kind::Bind, 1, 0, true, queue);
    EventFor(m, id, Kind::Clear, 1, 0, true, queue);
    EventFor(m, id, Kind::Draw, draws, 0, true, queue);
}
Frame Present(Model& m, bool unknown = false, int64_t now = 1000) { return m.Present(1920, 1080, 1, unknown, now); }
void Refuse(Reason reason, auto alter)
{
    Model m;
    auto id = Make(m);
    Scene(m, id);
    alter(m, id);
    auto f = Present(m);
    Check(f.r1.reason == reason);
    Check(f.r2.reason == reason);
}
} // namespace
int main()
{
    Check(Enabled(true, 2, "log"));
    for (auto v : { "auto", "constant", "observed", "LOG", "bad", "" })
        Check(!Enabled(true, 2, v));
    Check(!Enabled(false, 2, "log"));
    Check(!Enabled(true, 1, "log"));
    Check(!Enabled(true, 0, "log"));
    {
        Model m;
        auto scene = Make(m);
        auto shadow = Make(m, ShapeFor(2048, 2048));
        Scene(m, scene, 100);
        Scene(m, shadow, 900);
        auto f = Present(m);
        Check(f.r1.Accepted() && f.r1.pick.id == scene);
        Check(f.r2.pick.id == scene);
        Check(f.boundCount == 2 && f.extentCount == 1);
        Check(f.placement[0] == Placement::Committed && f.placement[1] == Placement::Committed);
        Check(f.r1.runner.id == shadow);
        auto detail = PickLine(f);
        Check(detail.find("R32_TYPELESS (DSV D32_FLOAT), committed, 1 sample") != std::string::npos);
        Check(detail.find("(reversed), state DEPTH_WRITE (creation, present queue)") != std::string::npos);
        Check(detail.find("runner-up 2048x2048") != std::string::npos);
        Check(detail.find("not at the backbuffer's extent") != std::string::npos);
        Check(Present(m).r1.reason == Reason::None);
        Check(Present(m).epoch == 3); // clock advances with no upscaler
    }
    {
        Model m;
        auto scene = Make(m), preview = Make(m);
        Scene(m, scene, 100);
        Scene(m, preview, 10);
        auto f = Present(m);
        Check(f.r1.pick.id == preview && f.r1.Accepted());
        Check(f.r2.pick.id == scene && f.r2.Accepted());
        Scene(m, scene, 100);
        Scene(m, preview, 50);
        Check(Present(m).r2.Accepted());
        Scene(m, scene, 100);
        Scene(m, preview, 51);
        f = Present(m);
        Check(f.r2.reason == Reason::Tie);
        Check(f.r1.Accepted());
        Scene(m, scene, 100);
        Scene(m, preview, 100);
        Check(Present(m).r2.reason == Reason::Tie);
    }
    {
        Model m;
        Check(Present(m).r1.reason == Reason::None);
        auto array = ShapeFor();
        array.singleSlice = false;
        auto a = Make(m, array);
        Scene(m, a);
        Check(Present(m).r1.reason == Reason::None);
        auto wrong = Make(m, ShapeFor(1280, 720));
        Scene(m, wrong);
        Check(Present(m).r1.reason == Reason::Extent);
        auto msaa = Make(m, ShapeFor(1920, 1080, 4));
        Scene(m, msaa);
        Check(Present(m).r1.reason == Reason::Multisampled);
        auto unknown = Make(m, ShapeFor(), false);
        Scene(m, unknown);
        Check(Present(m).r1.reason == Reason::Unobserved);
        EventFor(m, unknown, Kind::Bind);
        Check(Present(m).r2.reason == Reason::Unobserved);
        auto noState = Make(m, ShapeFor(), true, false);
        Scene(m, noState);
        Check(Present(m).r1.reason == Reason::State);
    }
    Refuse(Reason::Direction,
           [](Model& m, Id id)
           {
               EventFor(m, id, Kind::Clear, 1, 0.5f);
               EventFor(m, id, Kind::Draw, 10);
           });
    Refuse(Reason::Direction,
           [](Model& m, Id id)
           {
               EventFor(m, id, Kind::Clear, 1, 0, false);
               EventFor(m, id, Kind::Draw, 10);
           });
    Refuse(Reason::State, [](Model& m, Id id) { EventFor(m, id, Kind::Enhanced); });
    // R2's zero-draw margin deliberately precedes the content refusals; R1 must still diagnose the clear.
    {
        Model m;
        auto id = Make(m);
        Scene(m, id);
        EventFor(m, id, Kind::Clear);
        auto f = Present(m);
        Check(f.r1.reason == Reason::Cleared);
        Check(f.r2.reason == Reason::Tie);
    }
    Refuse(Reason::Aliased, [](Model& m, Id id) { EventFor(m, id, Kind::Alias); });
    Refuse(Reason::OtherQueue, [](Model& m, Id id) { EventFor(m, id, Kind::Draw, 1, 0, true, 2); });
    Refuse(Reason::Unobserved, [](Model& m, Id id) { EventFor(m, id, Kind::Unsupported); });
    Refuse(Reason::Unobserved, [](Model& m, Id id) { m.Forget(id); });
    {
        Model m;
        auto id = Make(m);
        Scene(m, id);
        Event e;
        e.kind = Kind::Transition;
        e.id = id;
        e.before = 16;
        e.after = 32;
        e.complete = true;
        m.Apply(e, 1);
        auto f = Present(m);
        Check(f.r1.Accepted());
        Check(f.r1.pick.source == StateSource::Barrier && f.r1.pick.state == 32);
        Scene(m, id);
        e.before = 16;
        m.Apply(e, 1);
        Check(Present(m).r1.reason == Reason::State);
    }
    for (bool split : { false, true })
    {
        Model m;
        auto id = Make(m);
        Scene(m, id);
        Event e;
        e.kind = Kind::Transition;
        e.id = id;
        e.before = 16;
        e.after = 32;
        e.complete = !split;
        m.Apply(e, 1);
        Check(Present(m).r1.reason == (split ? Reason::State : Reason::Accepted));
    }
    {
        Model m;
        auto a = m.Create(ShapeFor(), true, true, 16, 5), b = m.Create(ShapeFor(), true, true, 16, 5);
        Scene(m, a, 40);
        Scene(m, b, 10);
        EventFor(m, a, Kind::Alias);
        Check(Present(m).r1.reason == Reason::Aliased);
        Scene(m, a);
        EventFor(m, 0, Kind::Alias);
        Check(Present(m).r1.reason == Reason::Aliased);
        Scene(m, a);
        auto f = Present(m, true);
        Check(f.r1.reason == Reason::Unobserved && f.r2.reason == Reason::Unobserved);
        Check(m.Find(a) != nullptr); // preserved, not cleared because a list was unknown
        m.UnknownSubmission();
        Scene(m, a);
        Check(Present(m).r1.reason == Reason::State);
    }
    {
        Model m;
        auto old = Make(m);
        m.Forget(old);
        Present(m);
        auto fresh = Make(m);
        Check(fresh != old);
        Check(m.Find(old) == nullptr);
        for (size_t n = 1; n < kResources; ++n)
            Check(Make(m) != 0);
        Check(Make(m) == 0);
        Check(m.Find(old) == nullptr);
    }
    {
        Recording r;
        Check(!r.Usable(1));
        r.Reset(1, true);
        Event e;
        e.id = 42;
        e.kind = Kind::Draw;
        e.count = 1;
        for (int i = 0; i < 100000; ++i)
            r.Add(e);
        Check(r.size == 1 && r.events[0].count == 100000);
        r.closed = true;
        Check(r.Usable(1));
        Check(!r.Usable(2));
        r.Reset(2, false);
        Check(!r.Usable(2));
        r.Reset(3, true);
        e.kind = Kind::Bind;
        for (size_t i = 0; i < kEvents + 1; ++i)
            r.Add(e);
        r.closed = true;
        Check(!r.Usable(3));
        r.Reset(4, true);
        r.closed = true;
        r.Add(e);
        Check(!r.Usable(4));
        std::array<Recording, 4> records;
        std::array<std::thread, 4> workers;
        for (size_t i = 0; i < 4; ++i)
            workers[i] = std::thread(
                [&, i]
                {
                    records[i].Reset(i + 1, true);
                    Event bind;
                    bind.kind = Kind::Bind;
                    bind.id = i + 1;
                    records[i].Add(bind);
                    records[i].closed = true;
                });
        for (auto& w : workers)
            w.join();
        Model model;
        for (size_t i = 0; i < records.size(); ++i)
        {
            auto id = Make(model);
            Check(id == i + 1);
            Scene(model, id);
        }
        for (size_t i : { 3u, 0u, 2u, 1u })
        {
            Check(records[i].Usable(i + 1));
            for (size_t j = 0; j < records[i].size; ++j)
                model.Apply(records[i].events[j], 1);
        }
        Check(Present(model).r1.pick.id == 2); // submitted last, despite recording on thread 1
        // A closed recording can be replayed; reset changes the token and excludes the earlier contents.
        model.Apply(records[3].events[0], 1);
        Check(Present(model).r1.pick.id == 4);
        records[3].Reset(50, true);
        records[3].closed = true;
        Check(records[3].size == 0 && !records[3].Usable(4) && records[3].Usable(50));
    }
    {
        AddressTable<8> table;
        Check(table.Get(123).id == 0);
        Check(table.Put(123, { 4, 55 }));
        Check(table.Get(123).id == 4);
        const auto copied = table.Get(123);
        Check(table.Put(456, copied));
        Check(table.Get(456).flags == 55);
        Check(table.Put(123, {}));
        Check(table.Get(123).id == 0);
        Check(table.Get(456).id == 4);
        std::atomic<bool> stop = false;
        std::atomic<bool> inconsistent = false;
        std::thread reader(
            [&]
            {
                while (!stop)
                {
                    auto v = table.Get(456);
                    if (v.id && v.flags != v.id + 51)
                        inconsistent = true;
                }
            });
        for (uint64_t n = 1; n < 50000; ++n)
            Check(table.Put(456, { n, n + 51 }));
        stop = true;
        reader.join();
        Check(!inconsistent);
        for (uintptr_t key = 1000; key < 1006; ++key)
            Check(table.Put(key, { 1, 2 }));
        Check(!table.Put(9999, { 1, 2 }));
    }
    {
        Model m;
        auto id = Make(m);
        Window w;
        std::string line, changed;
        auto frame = [&](int64_t t)
        {
            Scene(m, id);
            return Present(m, false, t);
        };
        auto f = frame(0);
        f.commits = 3;
        f.eligible = true;
        f.truth = id;
        f.inverted = true;
        Check(!w.Add(f, &line, &changed));
        Check(!changed.empty());
        changed.clear();
        f = frame(1999999999);
        Check(!w.Add(f, &line, &changed));
        Check(changed.empty());
        f = frame(2000000000);
        f.commits = 1;
        f.eligible = true;
        f.truth = id;
        f.inverted = false;
        Check(w.Add(f, &line, &changed));
        Check(line.find("4 upscaler captures") != std::string::npos);
        Check(line.find("R1 picked 2 of 2 eligible comparisons, direction agrees 1") != std::string::npos);
        Check(line.find("R2 picked 2 of 2 eligible comparisons") != std::string::npos);
        Check(line.find("candidate scan (log only, nothing used)") != std::string::npos);
        Check(line.find("observer ") != std::string::npos);
        Check(line.find("(committed 1, placed 0, unknown placement 0)") != std::string::npos);
        // Another window with the same signature is held until 30 s.
        f = frame(3000000000);
        f.commits = 1;
        f.eligible = true;
        f.truth = id;
        f.inverted = true;
        Check(!w.Add(f, &line, &changed));
        f = frame(4000000000);
        f.commits = 1;
        f.eligible = true;
        f.truth = id;
        f.inverted = false;
        Check(!w.Add(f, &line, &changed));
        f = frame(6000000000);
        f.eligible = true;
        f.truth = id;
        f.inverted = true;
        Check(!w.Add(f, &line, &changed));
        f = frame(31999999999);
        Check(!w.Add(f, &line, &changed));
        f = frame(32000000000);
        Check(w.Add(f, &line, &changed));
    }
    {
        Window w;
        Frame f;
        std::string line, changed;
        Check(!w.Add(f, &line, &changed));
        f.nowNs = 100000000000LL;
        Check(!w.Add(f, &line, &changed)); // never a depth bind
        Model m;
        auto a = Make(m), b = Make(m);
        for (int i = 0; i < 5; ++i)
        {
            Scene(m, i % 2 ? a : b);
            f = Present(m, false, 200000000000LL + i * 1000000000LL);
            w.Add(f, &line, &changed);
        }
        Check(line.find("pick changed 1") != std::string::npos);
        // A new refusal category emits after >=2s, not per frame; backwards clock restarts the window.
        f.nowNs = 1;
        Check(!w.Add(f, &line, &changed));
    }
    {
        Model m;
        auto scene = Make(m), preview = Make(m);
        Window w;
        std::string line, changed;
        for (int i = 0; i < 3; ++i)
        {
            Scene(m, scene, 100);
            Scene(m, preview, i == 1 ? 51 : 10);
            auto f = Present(m, false, i * 1000000000LL);
            f.eligible = true;
            f.truth = scene;
            f.inverted = true;
            f.commits = 4; // commits can outnumber eligible comparisons
            w.Add(f, &line, &changed);
        }
        Check(line.find("12 upscaler captures") != std::string::npos);
        Check(line.find("R1 picked 0 of 3 eligible comparisons, direction agrees 0") != std::string::npos);
        Check(line.find("R2 picked 2 of 2 eligible comparisons, direction agrees 2") != std::string::npos);
        Check(line.find("R2 would pick 2, refuse:") != std::string::npos);
    }
    std::cout << "PASS: " << checks << " ranking, refusal, generation, recording, atomic lookup and window checks\n";
}
