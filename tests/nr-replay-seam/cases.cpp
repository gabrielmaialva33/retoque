// Behaviour of the replay seam against a scripted EvaluateAfterUpscale.
#include "DlssNrFeature_Dx12.h"
#include "DlssNr_Replay.h"
#include "DlssNr_ReplayClock.h"
#include "State.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

extern "C" uint32_t RetoqueReplay_Version();
extern "C" int RetoqueReplay_Begin();
extern "C" int RetoqueReplay_Evaluate(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, ID3D12CommandQueue*,
                                      const DlssNr::Replay::Frame*, DlssNr::Replay::Outcome*);
extern "C" void RetoqueReplay_End();

namespace
{
int failures = 0;
void Expect(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// What the fake pass does on its next call.
struct Script
{
    const char* skips[2] = { nullptr, nullptr };
    bool dispatch = false, carried = false;
    bool reenter = false;     // the pass calls RetoqueReplay_Evaluate again from inside
    bool foreignNote = false; // another thread notes while this evaluation runs
    int reentered = -1;
    uint32_t interval = 0, requested = 0, completed = 0;
    int32_t result = 0;
} script;
struct Seen
{
    int calls = 0;
    bool rayReconstruction = false;
    uint64_t frameCount = 0;
    std::chrono::steady_clock::time_point now {};
} seen;
} // namespace

namespace DlssNr
{
void EvaluateAfterUpscale(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, ID3D12CommandQueue*, bool,
                          bool isRayReconstruction)
{
    ++seen.calls;
    seen.rayReconstruction = isRayReconstruction;
    seen.frameCount = State::Instance().frameCount;
    seen.now = ReplayClock::Now();
    if (script.reenter)
    {
        Replay::Frame inner;
        inner.present = State::Instance().frameCount + 1;
        inner.timeMs = 1.0e6;
        Replay::Outcome innerOutcome;
        script.reentered = RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &inner, &innerOutcome);
    }
    if (script.foreignNote)
        std::thread([] { Replay::NoteSkip("noted from another thread"); }).join();
    for (const char* why : script.skips)
        if (why)
            Replay::NoteSkip(why);
    if (script.dispatch)
        Replay::NoteDispatch(true, script.carried, script.interval, script.requested, script.completed, script.result);
}
} // namespace DlssNr

int main()
{
    using namespace std::chrono;
    namespace RC = DlssNr::ReplayClock;

    // Outside a session the clock is the steady clock.
    Expect(!RC::Active(), "inactive at start");
    const auto a = steady_clock::now(), b = RC::Now(), c = steady_clock::now();
    Expect(a <= b && b <= c, "inactive Now() is the steady clock");

    // Notes outside a session change nothing a later session could see.
    DlssNr::Replay::NoteSkip("before any session");
    DlssNr::Replay::NoteDispatch(true, true, 4, 3, 3, 1);

    Expect(RetoqueReplay_Version() == 1, "ABI version 1");
    Expect(RetoqueReplay_Begin() == 0, "Begin refused without RETOQUE_REPLAY");
    setenv("RETOQUE_REPLAY", "yes", 1);
    Expect(RetoqueReplay_Begin() == 0, "Begin refused unless RETOQUE_REPLAY is exactly 1");
    DlssNr::Replay::Frame frame;
    DlssNr::Replay::Outcome outcome;
    Expect(RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &frame, &outcome) == 0 && seen.calls == 0,
           "Evaluate refused outside a session, pass not entered");
    setenv("RETOQUE_REPLAY", "1", 1);
    const auto before = steady_clock::now();
    Expect(RetoqueReplay_Begin() == 1 && RC::Active(), "Begin with RETOQUE_REPLAY=1");
    Expect(RetoqueReplay_Begin() == 0, "a second Begin is refused");

    // Version mismatches are refused before the pass is entered.
    frame.version = 2;
    Expect(RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &frame, &outcome) == 0 && seen.calls == 0,
           "frame version mismatch refused");
    frame.version = 1;
    outcome.version = 7;
    Expect(RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &frame, &outcome) == 0 && seen.calls == 0,
           "outcome version mismatch refused");
    outcome = {};

    // A model frame at 16.5 ms: present counter, logical clock, ray reconstruction and the outcome all arrive.
    frame.present = 4;
    frame.timeMs = 16.5;
    frame.rayReconstruction = 1;
    script = {};
    script.dispatch = true;
    script.interval = 1;
    script.requested = script.completed = 3;
    script.result = 1;
    Expect(RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &frame, &outcome) == 1 && seen.calls == 1,
           "Evaluate enters the pass once");
    Expect(seen.frameCount == 4 && seen.rayReconstruction, "present counter and RR placement reach the pass");
    const auto first = seen.now;
    Expect(first >= before && first - before < milliseconds(16) + milliseconds(250),
           "logical time is anchored at Begin");
    Expect(outcome.recorded == 1 && outcome.carried == 0 && outcome.interval == 1 && outcome.requestedPasses == 3 &&
               outcome.completedPasses == 3 && outcome.modelResult == 1 && outcome.skip[0] == '\0',
           "model frame outcome");

    // Real time passing does not move the logical clock; the frame's milliseconds do.
    std::this_thread::sleep_for(milliseconds(30));
    frame.present = 8;
    frame.timeMs = 33.0;
    frame.rayReconstruction = 0;
    script = {};
    script.dispatch = true;
    script.carried = true;
    script.interval = 4;
    script.requested = 3;
    Expect(RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &frame, &outcome) == 1, "second evaluation");
    Expect(seen.now - first == microseconds(16500), "logical clock advances by exactly the recorded delta");
    Expect(outcome.carried == 1 && outcome.interval == 4 && outcome.completedPasses == 0 && outcome.modelResult == 0,
           "carried frame outcome, nothing left over from the previous one");

    // A skipped evaluation: the first reason is kept, the outcome says nothing was recorded.
    frame.present = 12;
    frame.timeMs = 50.0;
    script = {};
    script.skips[0] = "chain settings are settling (500 ms)";
    script.skips[1] = "a later reason";
    Expect(RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &frame, &outcome) == 1, "skipped evaluation");
    Expect(std::string(outcome.skip) == "chain settings are settling (500 ms)" && outcome.recorded == 0 &&
               outcome.interval == 0,
           "first skip reason kept, nothing recorded");

    // A reason longer than the field is truncated, never overrun.
    std::string longReason(400, 'x');
    script = {};
    script.skips[0] = longReason.c_str();
    frame.present = 16;
    frame.timeMs = 66.0;
    Expect(RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &frame, &outcome) == 1 &&
               std::strlen(outcome.skip) == sizeof(outcome.skip) - 1,
           "long skip reason truncated");

    // Time and presents only move forward, and only finite, bounded times are taken; nothing is written on refusal.
    const int callsBefore = seen.calls;
    const auto refuse = [&](uint64_t present, double ms, const char* what)
    {
        DlssNr::Replay::Frame bad;
        bad.present = present;
        bad.timeMs = ms;
        Expect(RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &bad, &outcome) == 0 && seen.calls == callsBefore &&
                   State::Instance().frameCount == 16,
               what);
    };
    refuse(16, 70.0, "a repeated present is refused");
    refuse(15, 70.0, "a decreasing present is refused");
    refuse(17, 66.0, "a repeated time is refused");
    refuse(17, 65.0, "a decreasing time is refused");
    refuse(17, std::nan(""), "a NaN time is refused");
    refuse(17, INFINITY, "an infinite time is refused");
    refuse(17, 2.0e9, "a time beyond the bound is refused");

    // A nested evaluation from inside the pass is refused; the outer one completes with its own outcome.
    script = {};
    script.reenter = true;
    script.dispatch = true;
    script.interval = 2;
    script.requested = script.completed = 1;
    script.result = 1;
    frame.present = 20;
    frame.timeMs = 80.0;
    Expect(RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &frame, &outcome) == 1 && script.reentered == 0 &&
               outcome.interval == 2 && seen.frameCount == 20,
           "nested evaluation refused, outer outcome intact");

    // A note from another thread during an evaluation is not this evaluation's.
    script = {};
    script.foreignNote = true;
    frame.present = 24;
    frame.timeMs = 90.0;
    Expect(RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &frame, &outcome) == 1 &&
               std::string(outcome.skip) == "the pass returned without recording and gave no reason",
           "notes from another thread are ignored; a silent unrecorded evaluation still gets a reason");

    // Another thread can neither evaluate nor end the owner's session.
    int foreignResult = -1;
    std::thread(
        [&]
        {
            DlssNr::Replay::Frame f;
            f.present = 100;
            f.timeMs = 500.0;
            DlssNr::Replay::Outcome o;
            foreignResult = RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &f, &o);
            RetoqueReplay_End();
        })
        .join();
    Expect(foreignResult == 0 && RC::Active(), "a foreign thread cannot evaluate or end the session");

    // End: the clock is real again and the adapter refuses.
    RetoqueReplay_End();
    Expect(!RC::Active(), "End deactivates the clock");
    const auto d = steady_clock::now(), e = RC::Now(), f = steady_clock::now();
    Expect(d <= e && e <= f, "after End, Now() is the steady clock again");
    const int calls = seen.calls;
    Expect(RetoqueReplay_Evaluate(nullptr, nullptr, nullptr, &frame, &outcome) == 0 && seen.calls == calls,
           "Evaluate refused after End");
    // One session per process: the pass keeps the history the ended session built.
    Expect(RetoqueReplay_Begin() == 0, "no second session in one process");

    if (failures)
        return 1;
    std::printf("PASS 34 seam checks: clock inactive/anchored/logical/restored, environment gate, versions, "
                "outcome per evaluation, first skip kept and truncated, monotonic bounded clock, no nesting, "
                "owner thread only, one session per process\n");
    return 0;
}
