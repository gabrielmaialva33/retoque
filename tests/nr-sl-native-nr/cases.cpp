// The game's own Streamline Neural Rendering (retoque/dlssnr/DlssNr_SlNativeNr.h): what the Streamline hooks note, when
// Retoque's NR stands down for it, when it comes back, and what the log says. No Streamline, no D3D12, no game: the
// calls are the shapes slInit, slEvaluateFeature and slSetTag hand the hooks, written out as numbers.
#include "retoque/dlssnr/DlssNr_SlNativeNr.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace DlssNr::SlNativeNr;

namespace
{
// Streamline 2.11.1's ids (the vendored headers): DLSS, Reflex, PCL, DLSS-G, DLSS-RR, DirectSR.
constexpr uint32_t kOtherFeatures[] = { 0, 3, 4, 1000, 1001, 1003 };

// sl::ResourceTag's shape as far as NoteTags reads it: a type and a resource whose native pointer may be null.
struct FakeResource
{
    void* native = nullptr;
};
struct FakeTag
{
    FakeResource* resource = nullptr;
    uint32_t type = 0;
};

bool Has(const std::vector<std::string>& lines, const char* text)
{
    for (const auto& line : lines)
    {
        if (line.find(text) != std::string::npos)
            return true;
    }
    return false;
}

std::vector<std::string> Said(Detector& d, uint64_t nowMs, const char** reason = nullptr)
{
    std::vector<std::string> lines;
    const char* why = AskInto(d, lines, nowMs);
    if (reason != nullptr)
        *reason = why;
    return lines;
}
} // namespace

int main()
{
    unsigned checks = 0;

    // The constants, as NVIDIA-RTX/Streamline v2.14.1 include/sl_core_types.h declares them.
    {
        static_assert(kFeatureDlssNr == 1004);
        static_assert(kBufferTypeUpliftInputColor == 70);
        static_assert(kBufferTypeUpliftOutputColor == 71);
        static_assert(kBufferTypeUpliftControlMask == 72);
        static_assert(kQuietMs == 5000);
        ++checks;
    }

    // Nothing seen: nothing stands down, nothing is said.
    {
        Detector d;
        const Answer a = d.Ask(1000);
        assert(!a.standDown && !a.latchedNow && !a.releasedNow && a.standDowns == 0 && a.newlySeen == 0);
        assert(!d.Active() && d.SeenMask() == 0);
        assert(Lines(a).empty() && Reason(a) == nullptr);
        ++checks;
    }

    // slInit asking for sl.dlss_nr is the game being able to run it, not running it: said once, never a stand-down.
    {
        Detector d;
        const uint32_t without[] = { 0, 3, 1000 };
        d.NoteFeaturesToLoad(without, 3);
        d.NoteFeaturesToLoad(nullptr, 4);
        assert(d.SeenMask() == 0);

        const uint32_t with[] = { 0, 3, 1004, 1000 };
        d.NoteFeaturesToLoad(with, 4);
        assert(d.SeenMask() == kSeenRequested);

        const char* why = "unset";
        auto lines = Said(d, 1000, &why);
        assert(why == nullptr && lines.size() == 1 && Has(lines, "asked Streamline to load"));
        assert(Said(d, 1016).empty()); // once
        assert(!d.Active());
        ++checks;
    }

    // Every other feature's evaluate is not the game's NR, accepted or not.
    {
        Detector d;
        for (uint32_t feature : kOtherFeatures)
        {
            d.NoteEvaluate(feature, true, 1000);
            d.NoteEvaluate(feature, false, 1000);
        }
        assert(d.Evaluates() == 0 && d.Refused() == 0 && d.SeenMask() == 0);
        assert(!d.Ask(1000).standDown);
        ++checks;
    }

    // An evaluate Streamline refused (the plugin missing, the feature unsupported) did not run NR: counted and said
    // once, never a stand-down.
    {
        Detector d;
        d.NoteEvaluate(kFeatureDlssNr, false, 1000);
        d.NoteEvaluate(kFeatureDlssNr, false, 1016);
        assert(d.Refused() == 2 && d.Evaluates() == 0 && d.SeenMask() == kSeenRefused);
        const char* why = "unset";
        auto lines = Said(d, 1020, &why);
        assert(why == nullptr && lines.size() == 1 && Has(lines, "refused"));
        assert(!d.Active());
        ++checks;
    }

    // An accepted evaluate: the next ask stands down, latches and says why; every ask after it is counted.
    {
        Detector d;
        d.NoteEvaluate(kFeatureDlssNr, true, 1000);
        assert(d.Evaluates() == 1 && d.SeenMask() == kSeenEvaluated);

        const char* why = nullptr;
        auto lines = Said(d, 1000, &why);
        assert(why != nullptr && std::string(why) == kStandDownReason);
        assert(Has(lines, "runs its own Neural Rendering") && Has(lines, "stands down"));
        assert(d.Active() && d.StandDowns() == 1);

        const Answer second = d.Ask(1016);
        assert(second.standDown && !second.latchedNow && second.standDowns == 2 && Lines(second).empty());
        ++checks;
    }

    // The quiet window: still standing down kQuietMs after the game's last NR evaluate, released one millisecond
    // later, said once; latched again by the next evaluate.
    {
        Detector d;
        d.NoteEvaluate(kFeatureDlssNr, true, 1000);
        assert(d.Ask(1000).latchedNow);
        assert(d.Ask(1000 + kQuietMs).standDown);

        const Answer released = d.Ask(1000 + kQuietMs + 1);
        assert(!released.standDown && released.releasedNow && !d.Active());
        assert(released.standDowns == 2);
        auto lines = Lines(released);
        assert(lines.size() == 1 && Has(lines, "runs again") && Has(lines, "2 evaluates were stood down"));

        const Answer after = d.Ask(1000 + kQuietMs + 2);
        assert(!after.standDown && !after.releasedNow && Lines(after).empty());

        d.NoteEvaluate(kFeatureDlssNr, true, 20000);
        const Answer again = d.Ask(20010);
        assert(again.standDown && again.latchedNow && again.standDowns == 3);
        ++checks;
    }

    // A game that evaluates every frame keeps it latched however long it plays.
    {
        Detector d;
        uint64_t now = 1000;
        for (int frame = 0; frame < 2000; ++frame, now += 16)
        {
            d.NoteEvaluate(kFeatureDlssNr, true, now);
            const Answer a = d.Ask(now + 3);
            assert(a.standDown && !a.releasedNow);
            assert(a.latchedNow == (frame == 0));
        }
        assert(d.StandDowns() == 2000 && d.Evaluates() == 2000);
        ++checks;
    }

    // The hook thread's clock may run ahead of the asking thread's: an evaluate stamped after the ask still counts.
    {
        Detector d;
        d.NoteEvaluate(kFeatureDlssNr, true, 2000);
        const Answer a = d.Ask(1990);
        assert(a.standDown && a.latchedNow);
        ++checks;
    }

    // NR switched on long after the game's NR stopped: the old evaluate is past the window, nothing latches.
    {
        Detector d;
        d.NoteEvaluate(kFeatureDlssNr, true, 1000);
        const Answer a = d.Ask(1000 + kQuietMs + 100);
        assert(!a.standDown && !a.latchedNow && !a.releasedNow && a.standDowns == 0);
        auto lines = Lines(a);
        assert(lines.empty());
        ++checks;
    }

    // Uplift tags: the colour pair and the control mask are recorded (said once each); neither stands down, a tag with
    // no resource is no tag, and no other buffer type counts.
    {
        Detector d;
        d.NoteTag(kBufferTypeUpliftInputColor, false);
        d.NoteTag(kBufferTypeUpliftControlMask, false);
        for (uint32_t type : { 0u, 1u, 3u, 4u, 23u, 67u, 68u, 69u, 73u })
            d.NoteTag(type, true);
        assert(d.SeenMask() == 0);

        d.NoteTag(kBufferTypeUpliftOutputColor, true);
        assert(d.SeenMask() == kSeenColourTagged);
        auto lines = Said(d, 1000);
        assert(lines.size() == 1 && Has(lines, "uplift colour"));

        d.NoteTag(kBufferTypeUpliftInputColor, true);
        assert(Said(d, 1001).empty());

        d.NoteTag(kBufferTypeUpliftControlMask, true);
        assert(d.SeenMask() == (kSeenColourTagged | kSeenControlMask));
        const char* why = "unset";
        lines = Said(d, 1002, &why);
        assert(why == nullptr && lines.size() == 1 && Has(lines, "control mask") && Has(lines, "recorded only"));
        assert(!d.Active());
        ++checks;
    }

    // NoteTags reads sl::ResourceTag's own shape: a null array, a null resource and a null native pointer are skipped.
    {
        Detector d;
        NoteTags(d, static_cast<const FakeTag*>(nullptr), 3);
        FakeResource empty;
        FakeResource real;
        int texture = 0;
        real.native = &texture;
        const FakeTag tags[] = { { nullptr, kBufferTypeUpliftControlMask },
                                 { &empty, kBufferTypeUpliftControlMask },
                                 { &real, 1 },
                                 { &real, kBufferTypeUpliftInputColor } };
        NoteTags(d, tags, 3);
        assert(d.SeenMask() == 0);
        NoteTags(d, tags, 4);
        assert(d.SeenMask() == kSeenColourTagged);
        ++checks;
    }

    // Everything at once on the first ask: each observation said once, the latch line among them.
    {
        Detector d;
        const uint32_t with[] = { 1004 };
        d.NoteFeaturesToLoad(with, 1);
        d.NoteTag(kBufferTypeUpliftInputColor, true);
        d.NoteTag(kBufferTypeUpliftControlMask, true);
        d.NoteEvaluate(kFeatureDlssNr, false, 900);
        d.NoteEvaluate(kFeatureDlssNr, true, 1000);
        const char* why = nullptr;
        auto lines = Said(d, 1000, &why);
        assert(why != nullptr && lines.size() == 5);
        assert(Has(lines, "asked Streamline to load") && Has(lines, "refused") && Has(lines, "uplift colour") &&
               Has(lines, "control mask") && Has(lines, "stands down"));
        assert(Said(d, 1010).empty());
        ++checks;
    }

    // Counted and said at a bounded rate: one line per kSayEvery stand-downs after the first.
    {
        Detector d;
        unsigned periodic = 0;
        for (uint64_t i = 0; i < 3 * kSayEvery; ++i)
        {
            d.NoteEvaluate(kFeatureDlssNr, true, 1000 + i);
            const Answer a = d.Ask(1000 + i);
            for (const auto& line : Lines(a))
                periodic += line.find("stood down so far") != std::string::npos ? 1 : 0;
        }
        assert(periodic == 3);
        ++checks;
    }

    // The process's detector and the hooks' entry points: the same object, and a feature other than NR never reaches
    // the clock or the counters.
    {
        assert(&Instance() == &Instance());
        Instance().Reset();
        OnEvaluate(0, true);
        OnEvaluate(1000, true);
        assert(Instance().Evaluates() == 0);
        OnEvaluate(kFeatureDlssNr, true);
        assert(Instance().Evaluates() == 1);
        const uint32_t with[] = { 1004 };
        OnInit(with, 1);
        assert(Instance().SeenMask() == (kSeenEvaluated | kSeenRequested));
        std::vector<std::string> lines;
        assert(AskInto(Instance(), lines, NowMs()) == kStandDownReason);
        Instance().Reset();
        assert(Instance().Evaluates() == 0 && !Instance().Active() && Instance().SeenMask() == 0);
        ++checks;
    }

    // The hooks note from the game's threads while routes ask from theirs: every accepted evaluate is counted, every
    // ask after the first evaluate is a stand-down, and the latch is said exactly once.
    {
        Detector d;
        d.NoteEvaluate(kFeatureDlssNr, true, 1000);
        std::vector<std::thread> threads;
        std::atomic<unsigned> latches { 0 };
        for (int t = 0; t < 4; ++t)
            threads.emplace_back(
                [&d]()
                {
                    for (int i = 0; i < 1000; ++i)
                        d.NoteEvaluate(kFeatureDlssNr, true, 1000 + i);
                });
        for (int t = 0; t < 2; ++t)
            threads.emplace_back(
                [&d, &latches]()
                {
                    for (int i = 0; i < 1000; ++i)
                    {
                        const Answer a = d.Ask(1000 + i);
                        assert(a.standDown);
                        latches += a.latchedNow ? 1 : 0;
                    }
                });
        for (auto& thread : threads)
            thread.join();
        assert(d.Evaluates() == 4001 && d.StandDowns() == 2000 && latches == 1);
        ++checks;
    }

    std::printf("PASS: %u Streamline native-NR detector cases\n", checks);
    return 0;
}
