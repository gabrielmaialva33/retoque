// The NR minimum-driver gate (retoque/dlssnr/DlssNr_DriverGate.h), compiled as shipped under ASan/UBSan.
// Design: docs/dlssnr/design/driver-gate.md. Adapted from ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass 97f35314,
// 521138df and dec66857 (GPL-3.0); the rule is theirs, the shape here is Retoque's.

#include "DlssNr_DriverGate.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace DlssNr::DriverGate;

static int g_failures = 0;

static void Check(bool condition, const char* what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// 1. The rule, as a truth table. NVAPI encodes 610.88 as 61088.
static_assert(kMinimumWindowsDriver == 61088);
static_assert(kRecommendedWindowsDriver == 61664);
static_assert(kNgxResultOutOfDate == 0xBAD0000Cu); // NVSDK_NGX_Result_Fail | 12

static_assert(Resolve(true, false, 57688) == Verdict::TooOld); // DLSS Unlocked #60: RTX 2080 Ti, 576.88
static_assert(Resolve(true, false, 61087) == Verdict::TooOld);
static_assert(Resolve(true, false, 1) == Verdict::TooOld);
static_assert(Resolve(true, false, 61088) == Verdict::Supported);
static_assert(Resolve(true, false, 61663) == Verdict::Supported);
static_assert(Resolve(true, false, 61664) == Verdict::Supported);
static_assert(Resolve(true, false, 99999) == Verdict::Supported);

// 2. Unknown never blocks; Wine/Proton and other vendors are not gated at all.
static_assert(Resolve(true, false, 0) == Verdict::Unknown);
static_assert(Resolve(true, true, 57688) == Verdict::NotApplicable); // dxvk-nvapi: Linux numbering
static_assert(Resolve(true, true, 0) == Verdict::NotApplicable);
static_assert(Resolve(true, true, 61664) == Verdict::NotApplicable);
static_assert(Resolve(false, false, 57688) == Verdict::NotApplicable);
static_assert(Resolve(false, true, 57688) == Verdict::NotApplicable);
static_assert(Blocks(Verdict::TooOld));
static_assert(!Blocks(Verdict::Supported) && !Blocks(Verdict::Unknown) && !Blocks(Verdict::NotApplicable));

// 3. Only FAIL_OutOfDate is the driver's "too old"; its neighbours are not.
static_assert(IsOutOfDate(0xBAD0000Cu));
static_assert(!IsOutOfDate(0xBAD0000Bu)); // FAIL_UnableToInitializeFeature
static_assert(!IsOutOfDate(0xBAD0000Du)); // FAIL_OutOfGPUMemory
static_assert(!IsOutOfDate(1u));          // Success
static_assert(!IsOutOfDate(0u));
static_assert(!IsOutOfDate(static_cast<unsigned int>(-1)));

int main()
{
    // 4. Version text.
    Check(FormatVersion(61664) == "616.64", "616.64");
    Check(FormatVersion(57688) == "576.88", "576.88");
    Check(FormatVersion(60005) == "600.05", "leading zero of the minor kept");
    Check(FormatVersion(61100) == "611.00", "zero minor printed as 00");
    Check(FormatVersion(0) == "0.00", "zero");

    const std::string blocked = BlockedMessage(57688);
    Check(Contains(blocked, "610.88 or newer"), "message names the minimum");
    Check(Contains(blocked, "616.64 or newer recommended"), "message names the recommendation");
    Check(Contains(blocked, "this one is 576.88"), "message names the detected driver");
    Check(Contains(blocked, "settings are unchanged"), "message says the ini is untouched");

    // 5. The fixed reasons (the renderer keeps a const char*) carry the same numbers as the constants.
    const std::string tooOld = kTooOldReason;
    const std::string outOfDate = kOutOfDateReason;
    for (const auto* reason : { &tooOld, &outOfDate })
    {
        Check(Contains(*reason, FormatVersion(kMinimumWindowsDriver) + " or newer"), "reason names the minimum");
        Check(Contains(*reason, FormatVersion(kRecommendedWindowsDriver) + " or newer recommended"),
              "reason names the recommendation");
    }
    Check(Contains(outOfDate, "FAIL_OutOfDate"), "the latch reason names NGX's answer");

    // 6. A fresh gate refuses nothing and has nothing to say.
    {
        Gate gate;
        Check(!gate.Blocked() && !gate.OutOfDateSeen() && !gate.Refuses(), "fresh gate open");
        Check(gate.Version() == 0, "fresh gate has no version");
        Check(std::string(gate.Reason()).empty(), "fresh gate has no reason");
    }

    // 7. Publish: only a too-old verdict blocks, and it records the version for the menu.
    {
        Gate gate;
        gate.Publish(Verdict::TooOld, 57688);
        Check(gate.Blocked() && gate.Refuses() && gate.Version() == 57688, "too old blocks");
        Check(std::string(gate.Reason()) == kTooOldReason, "too old reason");

        for (const auto verdict : { Verdict::Supported, Verdict::Unknown, Verdict::NotApplicable })
        {
            Gate open;
            open.Publish(verdict, 61664);
            Check(!open.Blocked() && !open.Refuses(), "only TooOld blocks");
            Check(open.Version() == 61664, "version recorded whatever the verdict");
        }
    }

    // 8. The NGX latch: sticky once FAIL_OutOfDate is seen, untouched by any other result, including success later.
    {
        Gate gate;
        Check(!gate.Note(1u), "success does not latch");
        Check(!gate.Note(0xBAD0000Bu), "UnableToInitializeFeature does not latch");
        Check(!gate.Note(0u) && !gate.Refuses(), "zero does not latch");
        Check(gate.Note(0xBAD0000Cu), "OutOfDate latches and says so");
        Check(gate.OutOfDateSeen() && gate.Refuses() && !gate.Blocked(), "latched, not version-blocked");
        Check(!gate.Note(1u) && gate.Refuses(), "a later success does not unlatch");
        Check(std::string(gate.Reason()) == kOutOfDateReason, "latch reason");

        // The int the loaders keep (g_lastInit, g_lastCreate) converts to the same unsigned result.
        Gate fromInt;
        Check(fromInt.Note(static_cast<unsigned int>(static_cast<int>(0xBAD0000Cu))), "int round trip latches");
    }

    // 9. Both: the version block is the one named.
    {
        Gate gate;
        gate.Note(0xBAD0000Cu);
        gate.Publish(Verdict::TooOld, 57688);
        Check(std::string(gate.Reason()) == kTooOldReason, "the version block wins the reason");
    }

    // 10. The process gate is one object; the free functions are views of it.
    {
        Check(&Process() == &Process(), "one process gate");
        Check(!Refuses() && !Blocked(), "process gate starts open");
        Check(!NoteNgxResult(1u) && !Refuses(), "free Note on success");
        Publish(Verdict::Supported, 61664);
        Check(DetectedVersion() == 61664 && !Refuses(), "free Publish");
    }

    // 11. Concurrent publish and reads (the GPU worker against the render thread) under the sanitizers.
    {
        Gate gate;
        std::vector<std::thread> threads;
        threads.emplace_back([&] { gate.Publish(Verdict::TooOld, 57688); });
        for (int i = 0; i < 3; ++i)
            threads.emplace_back(
                [&]
                {
                    for (int n = 0; n < 1000; ++n)
                        (void) gate.Refuses();
                });
        threads.emplace_back([&] { gate.Note(0xBAD0000Cu); });
        for (auto& t : threads)
            t.join();
        Check(gate.Blocked() && gate.OutOfDateSeen(), "both states after the race");
    }

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("nr-driver-gate cases: all passed");
    return 0;
}
