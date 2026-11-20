// Mask inspection, slice 0: the production header against a fake config, clock and logger. No GPU, no D3D12.
#include "retoque/dlssnr/DlssNr_MaskInspection.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using namespace DlssNr::MaskInspection;

// Every heap allocation of the process is counted, so "off allocates nothing" is measured, not read off the source.
static size_t g_allocations = 0;

void* operator new(std::size_t size)
{
    ++g_allocations;
    if (void* memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

#define REQUIRE(condition)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (false)

namespace
{
template <class T> struct Option : std::optional<T>
{
    T fallback {};
    T value_or_default() const { return this->has_value() ? **this : fallback; }
};

struct FakeConfig
{
    Option<bool> DlssNrEnabled;
    Option<std::string> DlssNrMaskInspection;
};

FakeConfig ConfigWith(bool enabled, const char* inspection)
{
    FakeConfig cfg;
    cfg.DlssNrEnabled.emplace(enabled);
    cfg.DlssNrMaskInspection.fallback = "off";
    if (inspection != nullptr)
        cfg.DlssNrMaskInspection.emplace(inspection);
    return cfg;
}

uint64_t g_now = 0;
int g_clockReads = 0;
std::vector<std::string> g_lines;
bool g_sayThrows = false;

uint64_t Clock()
{
    ++g_clockReads;
    return g_now;
}

void Say(const char* line)
{
    if (g_sayThrows)
        throw std::runtime_error("the logger failed");
    g_lines.emplace_back(line);
}

bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// A frame of the present route the resolve recorded, with the defaults a user starts from.
Snapshot Recorded(uint64_t sequence)
{
    Snapshot s;
    s.sequence = sequence;
    s.route = Route::Present;
    s.outcome = Outcome::Recorded;
    s.modelGeneration = 1;
    s.width = 1920;
    s.height = 1080;
    s.requestedPasses = s.evaluatedPasses = 1;
    s.passes[0] = { Centi(-1.0f), true };
    s.ui = Ui::Off;
    s.detail = Centi(1.0f);
    s.maxRatio = Centi(2.0f);
    s.applyModel = true;
    s.passthrough = true;
    return s;
}

// One Dispatch through the production Scope, the way the pass uses it.
struct Bench
{
    Observer observer;

    bool Frame(bool requested, const Snapshot* frame, bool restart = false)
    {
        const size_t before = g_lines.size();
        {
            Scope scope(observer, requested, &Clock, &Say);
            if (scope && restart)
                scope.Restart();
            if (scope && frame != nullptr)
                *scope = *frame;
        }
        return g_lines.size() != before;
    }
    bool On(const Snapshot& frame) { return Frame(true, &frame); }
};

void Reset()
{
    g_now = 0;
    g_clockReads = 0;
    g_lines.clear();
    g_sayThrows = false;
}

void RequestIsExactlyLog()
{
    REQUIRE(IsLog("log"));
    for (const char* other : { "", "off", "auto", "LOG", "Log", "log ", " log", "log,overlay", "true", "1", "logs" })
        REQUIRE(!IsLog(other));

    REQUIRE(Requested(ConfigWith(true, "log")));
    REQUIRE(!Requested(ConfigWith(false, "log"))); // NR disabled: nothing is asked for
    REQUIRE(!Requested(ConfigWith(true, nullptr)));
    REQUIRE(!Requested(ConfigWith(true, "off")));
    REQUIRE(!Requested(ConfigWith(true, "overlay")));

    // A malformed value of any length is compared, never copied.
    const FakeConfig huge = ConfigWith(true, std::string(1 << 16, 'x').c_str());
    const size_t before = g_allocations;
    for (int i = 0; i < 100; ++i)
        REQUIRE(!Requested(huge));
    REQUIRE(g_allocations == before);
}

void OffIsInert()
{
    Reset();
    Bench bench;
    const Observer untouched = bench.observer;
    const Snapshot frame = Recorded(7);
    RouteNotice notice;

    for (const FakeConfig& cfg :
         { ConfigWith(true, nullptr), ConfigWith(true, "off"), ConfigWith(true, "overlay"), ConfigWith(false, "log") })
    {
        const bool requested = Requested(cfg);
        REQUIRE(!requested);
        const size_t before = g_allocations;
        for (int i = 0; i < 1000; ++i)
        {
            g_now += 5000; // a clock that would let every line through
            REQUIRE(!bench.Frame(requested, &frame, true));
            REQUIRE(!notice.Take(requested));
        }
        REQUIRE(g_allocations == before);
    }
    REQUIRE(g_clockReads == 0);
    REQUIRE(g_lines.empty());
    REQUIRE(std::memcmp(&bench.observer, &untouched, sizeof(Observer)) == 0);
    REQUIRE(!bench.observer.Live() && bench.observer.Generation() == 0 && bench.observer.Lines() == 0);

    // The notice a Vulkan route gives was not spent by asking while off.
    REQUIRE(notice.Take(true));
}

void FirstLineSaysWhatWasRecorded()
{
    Reset();
    Bench bench;
    g_now = 1000;
    REQUIRE(bench.On(Recorded(42)));
    const std::string& line = g_lines.back();
    for (const char* part :
         { "DLSS-NR mask inspection", "log only", "not GPU-completed", "no pixel read", "route d3d12-present",
           "frame 42", "generation 1", "model generation 1", "working 1920x1080", "model call recorded (1 of 1 pass)",
           "UI alpha: not handed: UiProtection is off", "pass 1 AutoMask on, SkinStructure follows LocalStructure",
           "the model's skin mask is internal and not exposed", "the model's edit is composed",
           "luminance guard applies (2.00x)", "residual limit off",
           "highlight recovery does not apply (the frame arrived tone mapped)", "darkening floor off",
           "not how many pixels took them" })
        REQUIRE(Has(line, part));
    REQUIRE(!Has(line, "line budget"));
    REQUIRE(bench.observer.Live() && bench.observer.Generation() == 1 && bench.observer.Lines() == 1);
}

void UnchangedStatusIsSaidOnce()
{
    Reset();
    Bench bench;
    REQUIRE(bench.On(Recorded(1)));
    const size_t before = g_allocations;
    for (uint64_t i = 2; i < 20000; ++i)
    {
        g_now += 16;
        REQUIRE(!bench.On(Recorded(i))); // the frame number alone is not news
    }
    REQUIRE(g_allocations == before); // observing allocates nothing; only a line that is said can
    REQUIRE(g_lines.size() == 1 && bench.observer.Generation() == 1);
}

void ChangesAreCoalescedToOneLineEveryTwoSeconds()
{
    Reset();
    Bench bench;
    g_now = 10000;
    REQUIRE(bench.On(Recorded(1)));

    Snapshot handed = Recorded(2);
    handed.ui = Ui::Handed;
    g_now = 10500;
    REQUIRE(!bench.On(handed));
    Snapshot unproduced = Recorded(3);
    unproduced.ui = Ui::NotProduced;
    g_now = 11999;
    REQUIRE(!bench.On(unproduced));
    unproduced.sequence = 4;
    g_now = 12000;
    REQUIRE(bench.On(unproduced)); // the latest answer, with the frame it was recorded on
    REQUIRE(Has(g_lines.back(), "frame 4") && Has(g_lines.back(), "the UI mask pass recorded nothing"));
    REQUIRE(!Has(g_lines.back(), "handed to pass 1"));

    // A change that went back before the gap ended is not said at all.
    g_now = 12100;
    REQUIRE(!bench.On(handed));
    g_now = 14100;
    REQUIRE(!bench.On(unproduced));
    REQUIRE(g_lines.size() == 2);

    // A status that changes on every frame for 100 s is one line per gap.
    Reset();
    Bench noisy;
    for (uint64_t frame = 0; frame < 6000; ++frame)
    {
        g_now = frame * 100 / 6; // 60 frames a second
        Snapshot s = Recorded(frame);
        s.maxRatio = Centi(1.0f + static_cast<float>(frame % 7));
        noisy.On(s);
    }
    REQUIRE(g_lines.size() >= 49 && g_lines.size() <= 51);
}

void AClockThatWentBackWaitsOneGap()
{
    Reset();
    Bench bench;
    g_now = 100000;
    REQUIRE(bench.On(Recorded(1)));
    Snapshot changed = Recorded(2);
    changed.ui = Ui::Handed;
    g_now = 5000;
    REQUIRE(!bench.On(changed)); // not early
    g_now = 6999;
    REQUIRE(!bench.On(changed));
    g_now = 7000;
    REQUIRE(bench.On(changed)); // and not silent until the old time comes round again
}

void OtherRoutesSayUnavailableOnce()
{
    for (const Route route :
         { Route::Bridge, Route::BeforeUpscale, Route::AfterRayReconstruction, Route::AfterUpscale })
    {
        Reset();
        Bench bench;
        Snapshot s = Recorded(9); // whatever the pass wrote, an unobserved route states none of it
        s.route = route;
        s.ui = Ui::Handed;
        REQUIRE(!Observed(route));
        REQUIRE(bench.On(s));
        const std::string line = g_lines.back();
        REQUIRE(Has(line, RouteName(route)) && Has(line, "unavailable on this route"));
        REQUIRE(Has(line, "no other source is looked for"));
        for (const char* absent : { "UI alpha", "AutoMask", "working", "guard", "recorded (" })
            REQUIRE(!Has(line, absent));
        for (uint64_t i = 0; i < 1000; ++i)
        {
            g_now += 2500;
            s.sequence = 10 + i;
            s.width = 100 + static_cast<uint32_t>(i);
            REQUIRE(!bench.On(s));
        }
    }
    REQUIRE(Observed(Route::Present));
    REQUIRE(RouteOf(true, false, false, false) == Route::Present);
    REQUIRE(RouteOf(true, true, false, false) == Route::Bridge);
    REQUIRE(RouteOf(false, false, true, true) == Route::BeforeUpscale);
    REQUIRE(RouteOf(false, false, false, true) == Route::AfterRayReconstruction);
    REQUIRE(RouteOf(false, true, false, false) == Route::AfterUpscale);
    REQUIRE(std::string(RouteName(Route::Present)) == "d3d12-present");
}

void UiAlphaIsHandedOrSaysWhyNot()
{
    Reset();
    Bench bench;
    Snapshot s = Recorded(1);
    s.ui = Ui::Handed;
    REQUIRE(bench.On(s));
    REQUIRE(Has(g_lines.back(), "UI alpha: handed to pass 1 as UI and UIAlpha (coverage unknown)"));

    s.ui = Ui::NotProduced;
    g_now += 2000;
    REQUIRE(bench.On(s));
    REQUIRE(Has(g_lines.back(), "UI alpha: not handed: the UI mask pass recorded nothing"));

    s.ui = Ui::Off;
    g_now += 2000;
    REQUIRE(bench.On(s));
    REQUIRE(Has(g_lines.back(), "UI alpha: not handed: UiProtection is off"));

    // A recording whose UI decision was never reached cannot be described: the sample is skipped.
    s.ui = Ui::NotDecided;
    g_now += 2000;
    REQUIRE(!bench.On(s));
    REQUIRE(bench.observer.Generation() == 1 && g_lines.size() == 3);
}

void SkinSettingsArePerPassAndTheMaskIsNotExposed()
{
    Reset();
    Bench bench;
    Snapshot s = Recorded(1);
    s.requestedPasses = 4;
    s.evaluatedPasses = 3;
    s.passes[0] = { Centi(0.5f), true };
    s.passes[1] = { Centi(0.5f), true };
    s.passes[2] = { Centi(-1.0f), false };
    s.passes[3] = { Centi(1.75f), true }; // requested, not evaluated: not said
    REQUIRE(bench.On(s));
    const std::string line = g_lines.back();
    REQUIRE(Has(line, "model call recorded (3 of 4 passes)"));
    REQUIRE(Has(line, "passes 1-2 AutoMask on, SkinStructure 0.50; pass 3 AutoMask off, SkinStructure follows "
                      "LocalStructure; the model's skin mask is internal and not exposed"));
    REQUIRE(!Has(line, "1.75") && !Has(line, "pass 4"));

    // A setting of a pass that did not run is not news either.
    s.passes[3] = { Centi(0.25f), false };
    g_now += 5000;
    REQUIRE(!bench.On(s));

    // A non-finite value is one stable value, said as such.
    s.passes[2] = { Centi(std::numeric_limits<float>::quiet_NaN()), true };
    g_now += 5000;
    REQUIRE(bench.On(s));
    REQUIRE(Has(g_lines.back(), "pass 3 AutoMask on, SkinStructure not finite"));
    g_now += 5000;
    REQUIRE(!bench.On(s));

    REQUIRE(Centi(0.5f) == 50 && Centi(-1.0f) == -100 && Centi(0.004f) == 0 && Centi(0.006f) == 1);
    REQUIRE(Centi(1.0e30f) == 100000000 && Centi(-1.0e30f) == -100000000);
    REQUIRE(Centi(std::numeric_limits<float>::infinity()) == kNotFinite);
    REQUIRE(Centi(std::numeric_limits<float>::quiet_NaN()) == kNotFinite);
}

void AFailedRecordingIsNeverDescribedByAnEarlierOne()
{
    // The pass declines from the start.
    Reset();
    Bench bench;
    Snapshot declined;
    declined.sequence = 5;
    REQUIRE(bench.On(declined));
    REQUIRE(Has(g_lines.back(), "nothing recorded for this frame") && Has(g_lines.back(), "frame 5"));

    // A Dispatch that returned early after writing part of its snapshot states none of it.
    Reset();
    Bench partial;
    Snapshot half = Recorded(6);
    half.outcome = Outcome::NotRecorded;
    half.ui = Ui::Handed;
    REQUIRE(partial.On(half));
    for (const char* absent : { "UI alpha", "AutoMask", "working", "guard" })
        REQUIRE(!Has(g_lines.back(), absent));

    // A Scope nothing was written to publishes exactly that.
    Reset();
    Bench untouched;
    REQUIRE(untouched.Frame(true, nullptr));
    REQUIRE(Has(g_lines.back(), "nothing recorded for this frame"));

    // A decline between two recordings (a second Dispatch refused at the same present) is not the status...
    Reset();
    Bench interleaved;
    REQUIRE(interleaved.On(Recorded(1)));
    for (uint64_t i = 0; i < 2000; ++i)
    {
        g_now += 8;
        declined.sequence = i;
        REQUIRE(!((i & 1) ? interleaved.On(Recorded(i)) : interleaved.On(declined)));
    }
    REQUIRE(g_lines.size() == 1 && interleaved.observer.Generation() == 1);

    // ...and one that lasts is, in its own words, with its own frame.
    const uint64_t lastRecording = g_now;
    declined.sequence = 9000;
    g_now = lastRecording + 1999;
    REQUIRE(!interleaved.On(declined));
    declined.sequence = 9001;
    g_now = lastRecording + 2000;
    REQUIRE(interleaved.On(declined));
    REQUIRE(Has(g_lines.back(), "nothing recorded for this frame") && Has(g_lines.back(), "frame 9001"));
    REQUIRE(!Has(g_lines.back(), "UI alpha") && interleaved.observer.Generation() == 1);

    // The driver proxy is said at once: nothing of an earlier recording stands in for it.
    Snapshot proxy;
    proxy.outcome = Outcome::DriverProxy;
    g_now += 100;
    REQUIRE(!interleaved.On(Recorded(9002)));
    g_now += 2000;
    REQUIRE(interleaved.On(Recorded(9003)));
    g_now += 1900;
    REQUIRE(!interleaved.On(Recorded(9004)));
    g_now += 100; // a recording 100 ms old, and the gap since the last line over
    REQUIRE(interleaved.On(proxy));
    REQUIRE(Has(g_lines.back(), "unavailable with the driver proxy") && !Has(g_lines.back(), "UI alpha"));
}

void SwitchingOffCancelsWhatWasPending()
{
    Reset();
    Bench bench;
    g_now = 1000;
    REQUIRE(bench.On(Recorded(1)));
    Snapshot changed = Recorded(2);
    changed.ui = Ui::Handed;
    g_now = 1100;
    REQUIRE(!bench.On(changed)); // pending: inside the gap

    const int reads = g_clockReads;
    for (int i = 0; i < 100; ++i)
    {
        g_now += 1000;
        REQUIRE(!bench.Frame(false, &changed)); // off: publication stops, and time passing says nothing
    }
    REQUIRE(g_clockReads == reads && !bench.observer.Live() && g_lines.size() == 1);

    // On again, with the status the log already carries: a new generation says it again, from a new frame.
    REQUIRE(bench.On(Recorded(77)));
    REQUIRE(bench.observer.Generation() == 2);
    REQUIRE(Has(g_lines.back(), "generation 2") && Has(g_lines.back(), "frame 77"));
    REQUIRE(!Has(g_lines.back(), "handed to pass 1"));

    // A recording from before the switch never stands in for a decline after it.
    Reset();
    Bench stale;
    g_now = 1000;
    REQUIRE(stale.On(Recorded(1)));
    REQUIRE(!stale.Frame(false, nullptr));
    Snapshot declined;
    declined.sequence = 2;
    g_now = 3000;
    REQUIRE(stale.On(declined));
    REQUIRE(Has(g_lines.back(), "nothing recorded for this frame") && Has(g_lines.back(), "generation 2"));
}

void EachDiscontinuityStartsAGeneration()
{
    Reset();
    Bench bench;
    Snapshot s = Recorded(1);
    REQUIRE(bench.On(s));
    REQUIRE(bench.observer.Generation() == 1);

    const auto next = [&](const char* generation)
    {
        g_now += 2000;
        ++s.sequence;
        REQUIRE(bench.On(s));
        REQUIRE(Has(g_lines.back(), generation));
    };

    s.width = 2560; // a resize
    s.height = 1440;
    next("generation 2");
    REQUIRE(Has(g_lines.back(), "working 2560x1440"));

    s.modelGeneration = 2; // the feature was rebuilt (a device change, a preset)
    next("generation 3");

    s.requestedPasses = 2; // the chain's length
    s.evaluatedPasses = 2;
    s.passes[1] = s.passes[0];
    next("generation 4");

    s.route = Route::AfterUpscale; // the route
    next("generation 5");
    s.route = Route::Present;
    next("generation 6");

    // The pass was switched off and on: same status, said again as the first of its generation.
    g_now += 2000;
    ++s.sequence;
    REQUIRE(bench.Frame(true, &s, true));
    REQUIRE(Has(g_lines.back(), "generation 7"));
    REQUIRE(bench.observer.Generation() == 7);

    // And nothing else does: a setting is a change within the generation.
    s.ui = Ui::Handed;
    next("generation 7");
    g_now += 2000;
    REQUIRE(!bench.On(s));

    // A restart drops the recording held for a decline: a decline right after it is said as one.
    Reset();
    Bench held;
    g_now = 1000;
    REQUIRE(held.On(Recorded(1)));
    Snapshot declined;
    declined.sequence = 3;
    g_now = 2900;
    REQUIRE(!held.On(Recorded(2)));
    g_now = 3000; // the recording is 100 ms old, and the gap since the last line is over
    REQUIRE(held.Frame(true, &declined, true));
    REQUIRE(Has(g_lines.back(), "nothing recorded for this frame") && Has(g_lines.back(), "generation 2"));

    // The rate holds across generations: a context that changes on every frame is still one line per gap.
    Reset();
    Bench flapping;
    for (uint64_t frame = 0; frame < 6000; ++frame)
    {
        g_now = frame * 100 / 6;
        Snapshot f = Recorded(frame);
        f.width = 1000 + static_cast<uint32_t>(frame % 3);
        flapping.Frame(true, &f, frame % 5 == 0);
    }
    REQUIRE(g_lines.size() >= 49 && g_lines.size() <= 51);
}

void CarriedFramesAreTheSameStatus()
{
    Reset();
    Bench bench;
    Snapshot model = Recorded(1);
    model.ui = Ui::Handed;
    Snapshot carried = model;
    carried.outcome = Outcome::Carried;
    REQUIRE(bench.On(model));
    for (uint64_t i = 2; i < 2000; ++i)
    {
        g_now += 16;
        Snapshot& frame = (i & 1) ? carried : model;
        frame.sequence = i;
        REQUIRE(!bench.On(frame));
    }
    REQUIRE(bench.observer.Generation() == 1);

    // A line that happens to see a carried frame says so.
    carried.ui = Ui::Off;
    g_now += 2000;
    REQUIRE(bench.On(carried));
    REQUIRE(Has(g_lines.back(), "carried frame (no model call; the last edit, made by 1 of 1 pass)"));
    carried.ui = Ui::Handed;
    g_now += 2000;
    REQUIRE(bench.On(carried));
    REQUIRE(Has(g_lines.back(), "UI alpha: handed to the carry (coverage unknown)"));
}

void CompositionFollowsTheResolvesBranches()
{
    Snapshot s = Recorded(1);
    s.passthrough = false;
    s.highlightRecovery = true;
    s.residualLimit = Centi(1.5f);
    s.minRatio = Centi(0.5f);

    Composition c = Compose(s);
    REQUIRE(c.notShown == nullptr && !c.replace && c.guard && c.residualLimit && c.highlightRecovery &&
            c.darkeningFloor);

    Line line;
    REQUIRE(Format(s, 1, false, line));
    std::string text = line.data();
    for (const char* part :
         { "the model's edit is composed", "luminance guard applies (2.00x)", "residual limit applies (1.50 stops)",
           "highlight recovery applies", "darkening floor applies (0.50x)" })
        REQUIRE(Has(text, part));
    REQUIRE(!Has(text, "compare mode") && !Has(text, "debug view"));

    // Nothing of the model is shown, so nothing guards it.
    const auto notShown = [](Snapshot frame, const char* why)
    {
        const Composition composition = Compose(frame);
        REQUIRE(composition.notShown != nullptr && Has(composition.notShown, why));
        REQUIRE(!composition.guard && !composition.residualLimit && !composition.highlightRecovery &&
                !composition.darkeningFloor && !composition.replace);
        Line said;
        REQUIRE(Format(frame, 1, false, said));
        REQUIRE(Has(said.data(), "the model's edit is not shown (") && Has(said.data(), "so no guard applies"));
        REQUIRE(!Has(said.data(), "applies ("));
    };
    Snapshot off = s;
    off.applyModel = false;
    notShown(off, "ApplyModel is off");
    Snapshot zero = s;
    zero.detail = 0;
    notShown(zero, "detail strength 0");
    for (uint32_t view = 1; view <= 3; ++view)
    {
        Snapshot debug = s;
        debug.debugView = view;
        notShown(debug, "debug view");
    }

    // Strength zero reaches the composition while a comparison or a debug view is up, as the shader has it.
    zero.compare = 1;
    REQUIRE(Compose(zero).notShown == nullptr);
    REQUIRE(Format(zero, 1, false, line) && Has(line.data(), "compare mode 1 shows one side as it arrived"));
    zero.compare = 0;
    zero.debugView = 4; // not a view the shader knows: it composes
    REQUIRE(Compose(zero).notShown == nullptr && Compose(zero).guard);
    REQUIRE(Format(zero, 1, false, line) && Has(line.data(), "debug view 4"));
    Snapshot nan = s;
    nan.detail = kNotFinite;
    REQUIRE(Compose(nan).notShown == nullptr);

    // The replace modes take the model's picture whole: the guard and the limit are not on that path, the highlight
    // recovery and the darkening floor after it are.
    for (uint32_t mode : { 2u, 4u })
    {
        Snapshot replace = s;
        replace.reversibleMode = mode;
        c = Compose(replace);
        REQUIRE(c.notShown == nullptr && c.replace && !c.guard && !c.residualLimit && c.highlightRecovery &&
                c.darkeningFloor);
        REQUIRE(Format(replace, 1, false, line));
        text = line.data();
        REQUIRE(Has(text, "taken whole (replace mode)") && Has(text, "luminance guard does not apply (replace mode)"));
        REQUIRE(Has(text, "residual limit does not apply (replace mode)") && Has(text, "highlight recovery applies"));
    }
    for (uint32_t mode : { 0u, 1u, 3u })
    {
        Snapshot composed = s;
        composed.reversibleMode = mode;
        REQUIRE(Compose(composed).guard && !Compose(composed).replace);
    }

    // Each optional branch follows its own setting.
    Snapshot toneMapped = s;
    toneMapped.passthrough = true;
    REQUIRE(!Compose(toneMapped).highlightRecovery && Compose(toneMapped).guard);
    REQUIRE(Format(toneMapped, 1, false, line) && Has(line.data(), "does not apply (the frame arrived tone mapped)"));
    Snapshot noRecovery = s;
    noRecovery.highlightRecovery = false;
    REQUIRE(!Compose(noRecovery).highlightRecovery);
    REQUIRE(Format(noRecovery, 1, false, line) && Has(line.data(), "highlight recovery off"));
    Snapshot noLimit = s;
    noLimit.residualLimit = 0;
    REQUIRE(!Compose(noLimit).residualLimit && Compose(noLimit).darkeningFloor);
    Snapshot noFloor = s;
    noFloor.minRatio = 0;
    REQUIRE(!Compose(noFloor).darkeningFloor && Compose(noFloor).residualLimit);

    // Not a recording: no branch applies.
    Snapshot none = s;
    none.outcome = Outcome::NotRecorded;
    REQUIRE(Compose(none).notShown != nullptr && !Compose(none).guard);
}

void LinesAndTheirNumberAreBounded()
{
    // The longest line there is fits, whole.
    Snapshot worst = Recorded(std::numeric_limits<uint64_t>::max());
    worst.modelGeneration = std::numeric_limits<uint64_t>::max();
    worst.width = worst.height = std::numeric_limits<uint32_t>::max();
    worst.requestedPasses = worst.evaluatedPasses = kMaxPasses;
    for (uint32_t i = 0; i < kMaxPasses; ++i)
        worst.passes[i] = { i % 2 ? Centi(-1.0f) : Centi(-1.0e30f - static_cast<float>(i)), i % 2 == 0 };
    worst.ui = Ui::Handed;
    worst.passthrough = false;
    worst.highlightRecovery = true;
    worst.maxRatio = worst.residualLimit = worst.minRatio = Centi(-1.0e30f);
    worst.residualLimit = worst.minRatio = Centi(1.0e30f);
    worst.compare = worst.debugView = std::numeric_limits<uint32_t>::max();
    Line line;
    REQUIRE(Format(worst, std::numeric_limits<uint64_t>::max(), true, line));
    const size_t longest = std::strlen(line.data());
    REQUIRE(longest > 1000 && longest < kLineSize - 256);
    REQUIRE(Has(line.data(), "pass 10 ") && Has(line.data(), "line budget reached"));

    // Text that does not fit is cut and reported, never written past the end.
    Line full;
    Writer writer(full);
    for (int i = 0; i < 400; ++i)
        writer.Text("0123456789");
    REQUIRE(!writer.Complete() && std::strlen(full.data()) == kLineSize - 1);
    writer.Number(123).Fixed(-150);
    REQUIRE(std::strlen(full.data()) == kLineSize - 1);
    Line fixed;
    Writer(fixed).Fixed(-150).Text(" ").Fixed(5).Text(" ").Fixed(kNotFinite).Text(" ").Fixed(123456);
    REQUIRE(std::string(fixed.data()) == "-1.50 0.05 not finite 1234.56");

    // kMaxLines per process, the last of them saying so; after it nothing is observed at all.
    Reset();
    Bench bench;
    for (uint64_t i = 0; i < 2 * kMaxLines; ++i)
    {
        g_now += kMinGapMs;
        Snapshot s = Recorded(i);
        s.maxRatio = Centi(1.0f + static_cast<float>(i));
        const bool said = bench.On(s);
        REQUIRE(said == (i < kMaxLines));
        if (said)
            REQUIRE(Has(g_lines.back(), "line budget reached") == (i == kMaxLines - 1));
    }
    REQUIRE(g_lines.size() == kMaxLines && bench.observer.Lines() == kMaxLines);
    const int reads = g_clockReads;
    const Observer spent = bench.observer;
    Snapshot later = Recorded(1);
    later.route = Route::AfterUpscale;
    g_now += 100000;
    REQUIRE(!bench.Frame(true, &later, true));
    REQUIRE(g_clockReads == reads && std::memcmp(&bench.observer, &spent, sizeof(Observer)) == 0);
}

void UnsafeMetadataSkipsTheSample()
{
    Reset();
    Bench bench;
    REQUIRE(bench.On(Recorded(1)));
    const Observer before = bench.observer;

    std::vector<Snapshot> unsafe(6, Recorded(2));
    unsafe[0].evaluatedPasses = 2; // more than were asked for
    unsafe[1].requestedPasses = unsafe[1].evaluatedPasses = kMaxPasses + 1;
    unsafe[2].width = 0;
    unsafe[3].height = 0;
    unsafe[4].evaluatedPasses = 0;
    unsafe[5].requestedPasses = unsafe[5].evaluatedPasses = 0;
    for (const Snapshot& s : unsafe)
    {
        g_now += 5000;
        Snapshot normalized;
        REQUIRE(!Normalize(s, normalized));
        REQUIRE(!bench.On(s));
        REQUIRE(std::memcmp(&bench.observer, &before, sizeof(Observer)) == 0); // as if it had never been seen
    }
    REQUIRE(g_lines.size() == 1);
}

void TheLoggerCannotBreakTheFrame()
{
    Reset();
    Bench bench;
    g_sayThrows = true;
    REQUIRE(!bench.On(Recorded(1))); // the throw stays inside the Scope
    g_sayThrows = false;
    g_now += 2000;
    Snapshot changed = Recorded(2);
    changed.ui = Ui::Handed;
    REQUIRE(bench.On(changed));
}

void AVulkanRouteSaysItOnce()
{
    RouteNotice notice;
    REQUIRE(!notice.Take(false));
    REQUIRE(notice.Take(true));
    for (int i = 0; i < 100; ++i)
        REQUIRE(!notice.Take(true));
    REQUIRE(Has(kVulkanUnavailable, "DLSS-NR mask inspection") &&
            Has(kVulkanUnavailable, "unavailable on native Vulkan"));
}
} // namespace

int main()
{
    RequestIsExactlyLog();
    OffIsInert();
    FirstLineSaysWhatWasRecorded();
    UnchangedStatusIsSaidOnce();
    ChangesAreCoalescedToOneLineEveryTwoSeconds();
    AClockThatWentBackWaitsOneGap();
    OtherRoutesSayUnavailableOnce();
    UiAlphaIsHandedOrSaysWhyNot();
    SkinSettingsArePerPassAndTheMaskIsNotExposed();
    AFailedRecordingIsNeverDescribedByAnEarlierOne();
    SwitchingOffCancelsWhatWasPending();
    EachDiscontinuityStartsAGeneration();
    CarriedFramesAreTheSameStatus();
    CompositionFollowsTheResolvesBranches();
    LinesAndTheirNumberAreBounded();
    UnsafeMetadataSkipsTheSample();
    TheLoggerCannotBreakTheFrame();
    AVulkanRouteSaysItOnce();
    std::puts("PASS: mask inspection cases");
    return 0;
}
