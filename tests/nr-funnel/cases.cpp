// tests/nr-funnel: the NR engagement funnel (retoque/dlssnr/DlssNr_Funnel.h, docs/dlssnr/design/engagement-funnel.md).
// The header is standard library only and compiled as it is, under ASan/UBSan, with threads.

#include <dlssnr/DlssNr_Funnel.h>

#include <atomic>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
int g_failures = 0;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

namespace F = DlssNr::Funnel;

// The ladders are constant-initialised: a crash handler may read them before any constructor would have run, and no
// initialisation guard sits between it and the counters.
constinit F::Ladder g_constantLadder;

void FreshLadderBalances()
{
    F::Ladder ladder;
    const auto s = ladder.Read();
    Require(s.eligible == 0 && s.ran == 0 && s.modelCalls == 0 && s.Declined() == 0, "a fresh ladder reads zero");
    Require(s.Balanced() && s.Unaccounted() == 0, "a fresh ladder balances");
    F::Decline top {};
    uint64_t count = 7;
    Require(!s.TopDecline(&top, &count) && count == 0, "no top decline with nothing declined");
    Require(g_constantLadder.Read().Balanced(), "a constant-initialised ladder balances");
}

void EveryWayOutIsCounted()
{
    F::Ladder ladder;
    {
        F::Evaluation e(F::Route::D3d12AfterUpscale, 1, ladder);
        e.Declined(F::Decline::DeferredToPresent);
    }
    {
        F::Evaluation e(F::Route::D3d12AfterUpscale, 1, ladder);
        e.Ran(true);
    }
    {
        F::Evaluation e(F::Route::D3d12AfterUpscale, 1, ladder);
        e.Ran(false); // a carried frame: the pass recorded, the model was not called
    }
    {
        F::Evaluation e(F::Route::D3d12AfterUpscale, 1, ladder);
        e.Declined(F::Decline::PassRecordedNothing, "it is still building");
    }
    const auto s = ladder.Read();
    Require(s.eligible == 4, "four evaluates entered");
    Require(s.ran == 2 && s.modelCalls == 1, "two ran the pass, one called the model");
    Require(s.declines[static_cast<size_t>(F::Decline::DeferredToPresent)] == 1, "the deferral counted");
    Require(s.declines[static_cast<size_t>(F::Decline::PassRecordedNothing)] == 1, "the empty pass counted");
    Require(s.Declined() == 2, "two declined");
    Require(s.Balanced() && s.Unaccounted() == 0, "eligible - ran - declined = 0");
    Require(s.lastPassReason != nullptr && std::string_view(s.lastPassReason) == "it is still building",
            "the pass's own reason is kept with its decline");
}

void ASilentReturnBreaksTheBalance()
{
    F::Ladder ladder;
    {
        F::Evaluation e(F::Route::D3d12AfterUpscale, 1, ladder);
        // A way out that names no outcome: the bug the balance exists to show.
    }
    const auto s = ladder.Read();
    Require(s.eligible == 1 && s.Accounted() == 0, "an evaluate with no named outcome is not hidden in a bucket");
    Require(!s.Balanced() && s.Unaccounted() == 1, "and shows as unaccounted");
}

void FirstOutcomeWins()
{
    F::Ladder ladder;
    {
        F::Evaluation e(F::Route::D3d12AfterUpscale, 1, ladder);
        e.Declined(F::Decline::MultiView);
        Require(e.Ended(), "a decline ends the evaluation");
        e.Ran(true);
        e.Declined(F::Decline::NoDevice);
    }
    const auto s = ladder.Read();
    Require(s.Balanced() && s.ran == 0 && s.Declined() == 1 &&
                s.declines[static_cast<size_t>(F::Decline::MultiView)] == 1,
            "one evaluate, one outcome: later calls on an ended evaluation count nothing");
}

void NullDetailKeepsTheLastReason()
{
    F::Ladder ladder;
    ladder.Enter();
    ladder.Declined(F::Decline::PassRecordedNothing, "first");
    ladder.Enter();
    ladder.Declined(F::Decline::PassRecordedNothing, nullptr);
    Require(std::string_view(ladder.Read().lastPassReason) == "first", "a decline without detail keeps the last one");
}

void TopDecline()
{
    F::Ladder ladder;
    for (int i = 0; i < 3; ++i)
    {
        ladder.Enter();
        ladder.Declined(F::Decline::DeferredToPresent, nullptr);
    }
    ladder.Enter();
    ladder.Declined(F::Decline::Stage1BeforeUpscaler, nullptr);
    F::Decline top {};
    uint64_t count = 0;
    Require(ladder.Read().TopDecline(&top, &count) && top == F::Decline::DeferredToPresent && count == 3,
            "the top decline is the most frequent");
}

void StageMarks()
{
    F::Ladder ladder;
    {
        F::Evaluation e(F::Route::D3d12AfterUpscale, 0xABCDu, ladder);
        auto mark = F::LastStage();
        Require(mark.route == F::Route::D3d12AfterUpscale && mark.stage == F::Stage::Entered && mark.thread == 0xABCDu,
                "entering marks the route, the stage and the thread");
        e.Recording();
        mark = F::LastStage();
        Require(mark.stage == F::Stage::Recording && mark.thread == 0xABCDu, "recording the pass is marked");
        e.Ran(true);
    }
    const auto mark = F::LastStage();
    Require(mark.stage == F::Stage::Left && mark.thread == 0xABCDu, "leaving is marked");
    Require(std::string_view(F::StageText(F::Stage::Recording)).size() > 0 &&
                std::string_view(F::StageText(F::Stage::None)).size() > 0,
            "every stage has a text");
}

void NamesAreAContract()
{
    std::set<std::string> tags, texts;
    for (size_t i = 0; i < F::kDeclineCount; ++i)
    {
        const std::string tag = F::DeclineTag(static_cast<F::Decline>(i));
        const std::string text = F::DeclineText(static_cast<F::Decline>(i));
        Require(!tag.empty() && !text.empty(), "decline " + std::to_string(i) + " has a tag and a text");
        for (const char c : tag)
            Require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_', "a decline tag is [a-z0-9_]: " + tag);
        tags.insert(tag);
        texts.insert(text);
    }
    Require(tags.size() == F::kDeclineCount && texts.size() == F::kDeclineCount, "tags and texts are unique");
    for (size_t i = 0; i < F::kRouteCount; ++i)
        Require(std::string_view(F::RouteName(static_cast<F::Route>(i))).size() > 0 &&
                    std::string_view(F::RouteTag(static_cast<F::Route>(i))).size() > 0,
                "every route has a name and a tag");
    Require(std::string_view(F::DeclineTag(F::Decline::Count)) == "unknown", "an out-of-range decline is unknown");
}

void GlobalRegistry()
{
    const auto before = F::Read(F::Route::D3d12AfterUpscale);
    {
        F::Evaluation e(F::Route::D3d12AfterUpscale, 2);
        e.Declined(F::Decline::NoDevice);
    }
    const auto after = F::Read(F::Route::D3d12AfterUpscale);
    Require(after.eligible == before.eligible + 1 && after.declines[static_cast<size_t>(F::Decline::NoDevice)] ==
                                                         before.declines[static_cast<size_t>(F::Decline::NoDevice)] + 1,
            "the default ladder is the route's global one");
    Require(&F::For(F::Route::D3d12AfterUpscale) == &F::For(F::Route::D3d12AfterUpscale), "one ladder per route");
}

// Several render threads count while a reader (the panel, a crash note) snapshots. The reader never sees more outcomes
// than entries, so "unaccounted" is never negative; once the threads stop, the ladder balances exactly.
void ConcurrentCountingBalances()
{
    F::Ladder ladder;
    std::atomic<bool> stop { false };
    std::atomic<bool> overAccounted { false };
    constexpr int kThreads = 4, kEach = 50000;

    std::thread reader(
        [&]
        {
            while (!stop.load())
            {
                const auto s = ladder.Read();
                if (s.Unaccounted() < 0)
                    overAccounted = true;
            }
        });
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t)
        writers.emplace_back(
            [&, t]
            {
                for (int i = 0; i < kEach; ++i)
                {
                    F::Evaluation e(F::Route::D3d12AfterUpscale, static_cast<uint32_t>(t + 1), ladder);
                    if ((i + t) % 3 == 0)
                        e.Ran((i & 1) != 0);
                    else
                        e.Declined(static_cast<F::Decline>((i + t) % F::kDeclineCount));
                }
            });
    for (auto& w : writers)
        w.join();
    stop = true;
    reader.join();

    const auto s = ladder.Read();
    Require(!overAccounted.load(), "a concurrent snapshot never reads more outcomes than entries");
    Require(s.eligible == static_cast<uint64_t>(kThreads) * kEach, "every entry counted");
    Require(s.Balanced(), "the ladder balances once the threads stop");
}
} // namespace

int main()
{
    FreshLadderBalances();
    EveryWayOutIsCounted();
    ASilentReturnBreaksTheBalance();
    FirstOutcomeWins();
    NullDetailKeepsTheLastReason();
    TopDecline();
    StageMarks();
    NamesAreAContract();
    GlobalRegistry();
    ConcurrentCountingBalances();

    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS: funnel cases\n");
    return 0;
}
