// The window host's pure rules (retoque/window_host/{Geometry,Policy,Pacing,Protocol}.h), compiled as they ship.
#include "Geometry.h"
#include "Pacing.h"
#include "Policy.h"
#include "Protocol.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
int failures = 0, checks = 0;
void Expect(bool ok, const char* what)
{
    ++checks;
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
bool Same(const char* a, const char* b) { return a != nullptr && b != nullptr && std::strcmp(a, b) == 0; }

namespace G = RetoqueWindowHost::Geometry;
namespace P = RetoqueWindowHost::Policy;
namespace Pc = RetoqueWindowHost::Pacing;
namespace Pr = RetoqueWindowHost::Protocol;

void Geometry()
{
    // A 1280x720 client inside a frame with an 8 px border and a 31 px title bar, at (100, 200).
    G::CropInputs in;
    in.frameBounds = { 100, 200, 100 + 8 + 1280 + 8, 200 + 31 + 720 + 8 };
    in.client = { 108, 231, 108 + 1280, 231 + 720 };
    in.content = { 1296, 759 };
    G::Crop crop = G::ClientCrop(in);
    Expect(crop.ok && crop.box == G::Box { 8, 31, 1288, 751 }, "framed window: crop is the client inside the bounds");
    Expect(crop.box.Width() == 1280 && crop.box.Height() == 720, "crop extent is the client extent");

    // Borderless (a game in borderless fullscreen): bounds equal the client; the crop is the whole frame.
    in.frameBounds = in.client = { 0, 0, 1920, 1080 };
    in.content = { 1920, 1080 };
    crop = G::ClientCrop(in);
    Expect(crop.ok && crop.box == G::Box { 0, 0, 1920, 1080 }, "borderless: whole frame");

    // Negative screen coordinates (a monitor left of the primary) change nothing.
    in.frameBounds = { -1928, 100, -1928 + 1296, 100 + 759 };
    in.client = { -1920, 131, -1920 + 1280, 131 + 720 };
    in.content = { 1296, 759 };
    crop = G::ClientCrop(in);
    Expect(crop.ok && crop.box == G::Box { 8, 31, 1288, 751 }, "negative coordinates");

    // WGC still delivering the old raster after a resize: the size does not confirm the bounds; never guessed.
    in.content = { 1000, 700 };
    crop = G::ClientCrop(in);
    Expect(!crop.ok && Same(crop.reason, "the captured size does not confirm where the client area starts"),
           "stale size refused");

    // Magpie's maximized case: bounds reach 8 px past a 2560x1440 monitor (work area minus a 48 px taskbar); the
    // capture is the bounds cut to the work area, and the client starts below the work area's top.
    G::CropInputs max;
    max.maximized = true;
    max.monitorWorkKnown = true;
    max.monitorWork = { 0, 0, 2560, 1392 };
    max.frameBounds = { -8, -8, 2568, 1400 };
    max.client = { 0, 23, 2560, 1392 };
    max.content = { 2560, 1392 };
    crop = G::ClientCrop(max);
    Expect(crop.ok && crop.box == G::Box { 0, 23, 2560, 1392 }, "maximized: bounds cut to the work area");
    // ...and the variant where the client starts above the work area (its caption pushed off-screen): the capture is
    // the client itself.
    max.client = { 0, -2, 2560, 1390 };
    max.content = { 2560, 1392 };
    crop = G::ClientCrop(max);
    Expect(crop.ok && crop.box == G::Box { 0, 0, 2560, 1392 }, "maximized, client above the work area");
    // Not maximized: the correction never applies.
    max.maximized = false;
    crop = G::ClientCrop(max);
    Expect(!crop.ok, "the maximized correction needs a maximized window");
    max.maximized = true;
    max.monitorWorkKnown = false;
    crop = G::ClientCrop(max);
    Expect(!crop.ok, "the maximized correction needs the monitor's work area");

    // DWM draws no frame: the capture is the client area.
    G::CropInputs bare;
    bare.frameBounds = { 0, 0, 1300, 760 };
    bare.client = { 10, 10, 1290, 730 };
    bare.content = { 1280, 720 };
    bare.ncRenderingKnown = true;
    bare.ncRendering = false;
    crop = G::ClientCrop(bare);
    Expect(crop.ok && crop.box == G::Box { 0, 0, 1280, 720 }, "no DWM frame: the client is the capture");
    bare.ncRendering = true;
    Expect(!G::ClientCrop(bare).ok, "with a DWM frame that shortcut is refused");

    // Refusals.
    G::CropInputs bad = in;
    bad.content = { 0, 759 };
    Expect(Same(G::ClientCrop(bad).reason, "the captured frame is empty"), "empty frame");
    bad = in;
    bad.content = { 1296, 759 };
    bad.client = { 10, 10, 10, 50 };
    Expect(Same(G::ClientCrop(bad).reason, "the source window has no client area"), "empty client");
    bad.frameBounds = { 0, 0, 1296, 759 };
    bad.client = { 100, 100, 1400, 700 };
    Expect(Same(G::ClientCrop(bad).reason, "the client area lies outside the captured frame"), "client past bounds");
    bad.client = { -1, 0, 100, 100 };
    Expect(!G::ClientCrop(bad).ok, "client left of bounds");
    // Extreme coordinates do not overflow.
    bad.frameBounds = { INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX };
    bad.client = { 0, 0, 10, 10 };
    bad.content = { 1, 1 };
    Expect(!G::ClientCrop(bad).ok, "extreme bounds refused, no overflow");
}

P::WindowFacts Good()
{
    P::WindowFacts w;
    w.exists = w.topLevel = w.visible = w.cloakKnown = w.boundsKnown = w.hasTitle = w.affinityKnown = true;
    w.pid = 4242;
    w.selfPid = 1000;
    w.width = 1296;
    w.height = 759;
    w.className = "UnrealWindow";
    return w;
}

void Policy()
{
    Expect(P::CandidateRefusal(Good()) == nullptr, "an ordinary game window is a candidate");
    struct Case
    {
        const char* what;
        void (*mutate)(P::WindowFacts&);
        const char* reason;
        bool unavailableToo; // still refused when allowUnavailable
    };
    const Case cases[] = {
        { "gone", [](P::WindowFacts& w) { w.exists = false; }, "not a top-level window", true },
        { "child", [](P::WindowFacts& w) { w.topLevel = false; }, "not a top-level window", true },
        { "desktop", [](P::WindowFacts& w) { w.desktopOrShell = true; }, "the desktop and the shell are not captured",
          true },
        { "hidden", [](P::WindowFacts& w) { w.visible = false; }, "the window is hidden or minimized", false },
        { "minimized", [](P::WindowFacts& w) { w.minimized = true; }, "the window is hidden or minimized", false },
        { "transparent", [](P::WindowFacts& w) { w.transparent = true; },
          "transparent, tool and non-activating windows are not captured", true },
        { "tool", [](P::WindowFacts& w) { w.toolWindow = true; },
          "transparent, tool and non-activating windows are not captured", true },
        { "noactivate", [](P::WindowFacts& w) { w.noActivate = true; },
          "transparent, tool and non-activating windows are not captured", true },
        { "owned popup", [](P::WindowFacts& w) { w.owned = true; }, "owned pop-up windows are not captured", true },
        { "self", [](P::WindowFacts& w) { w.pid = w.selfPid; }, "the host's own windows are not captured", true },
        { "no pid", [](P::WindowFacts& w) { w.pid = 0; }, "the host's own windows are not captured", true },
        { "cloaked", [](P::WindowFacts& w) { w.cloaked = true; }, "the window is cloaked or unknown to the compositor",
          false },
        { "cloak unknown", [](P::WindowFacts& w) { w.cloakKnown = false; },
          "the window is cloaked or unknown to the compositor", true },
        { "narrow", [](P::WindowFacts& w) { w.width = 49; }, "the window is smaller than 50 pixels on a side", true },
        { "short", [](P::WindowFacts& w) { w.height = 10; }, "the window is smaller than 50 pixels on a side", true },
        { "no bounds", [](P::WindowFacts& w) { w.boundsKnown = false; },
          "the window is smaller than 50 pixels on a side", true },
        { "untitled", [](P::WindowFacts& w) { w.hasTitle = false; }, "the window has no title", true },
        { "preview", [](P::WindowFacts& w) { w.className = P::kPreviewClass; },
          "shell, pop-up and host windows are not captured", true },
        { "progman", [](P::WindowFacts& w) { w.className = "Progman"; },
          "shell, pop-up and host windows are not captured", true },
        { "workerw", [](P::WindowFacts& w) { w.className = "WorkerW"; },
          "shell, pop-up and host windows are not captured", true },
        { "taskbar", [](P::WindowFacts& w) { w.className = "Shell_TrayWnd"; },
          "shell, pop-up and host windows are not captured", true },
        { "affinity monitor", [](P::WindowFacts& w) { w.affinity = P::kAffinityMonitor; },
          "the window asks not to be captured (display affinity); it is never worked around", true },
        { "affinity exclude", [](P::WindowFacts& w) { w.affinity = P::kAffinityExcludeFromCapture; },
          "the window asks not to be captured (display affinity); it is never worked around", true },
    };
    for (const Case& c : cases)
    {
        P::WindowFacts w = Good();
        c.mutate(w);
        Expect(Same(P::CandidateRefusal(w), c.reason), c.what);
        const char* paused = P::CandidateRefusal(w, true);
        Expect(c.unavailableToo ? Same(paused, c.reason) : paused == nullptr, c.what);
    }
    P::WindowFacts appOwned = Good();
    appOwned.owned = appOwned.appWindow = true;
    Expect(P::CandidateRefusal(appOwned) == nullptr, "an owned window that asks for the taskbar is a candidate");
    P::WindowFacts unknownAffinity = Good();
    unknownAffinity.affinityKnown = false;
    unknownAffinity.affinity = P::kAffinityExcludeFromCapture;
    Expect(P::CandidateRefusal(unknownAffinity) == nullptr, "an unreadable affinity is not invented");

    // Identity.
    P::Identity a { 0x1234, 77, 133'000'000'000'000'000ull, "UnrealWindow", "C:\\Games\\Game.exe" };
    P::Identity b = a;
    Expect(P::SameSource(a, b), "same identity");
    b.processCreation += 1;
    Expect(!P::SameSource(a, b), "a reused handle in a new process is another source");
    b = a;
    b.className = "Other";
    Expect(!P::SameSource(a, b), "another class is another source");
    b = a;
    b.executable = "C:\\Other.exe";
    Expect(!P::SameSource(a, b), "another executable is another source");
    b = a;
    b.pid = 0;
    Expect(!P::SameSource(b, b), "an incomplete identity never matches, even itself");

    // Run, pause, stop.
    P::SourceFacts s;
    Expect(P::DecideSource(s).action == P::Action::Run, "a visible source runs");
    s.minimized = true;
    Expect(P::DecideSource(s).action == P::Action::Pause && Same(P::DecideSource(s).reason, "the source is minimized"),
           "minimized pauses");
    s = {};
    s.visible = false;
    Expect(P::DecideSource(s).action == P::Action::Pause, "hidden pauses");
    s = {};
    s.cloaked = true;
    Expect(P::DecideSource(s).action == P::Action::Pause, "cloaked pauses");
    s = {};
    s.itemClosed = true;
    s.minimized = true;
    Expect(P::DecideSource(s).action == P::Action::Stop, "a closed capture item stops, even while minimized");
    s = {};
    s.exists = false;
    Expect(Same(P::DecideSource(s).reason, "the source window closed"), "a closed window stops");
    s = {};
    s.sameIdentity = false;
    Expect(P::DecideSource(s).action == P::Action::Stop, "a changed identity stops");

    // Adapter.
    Expect(P::AdapterRefusal(true, P::kNvidia) == nullptr, "the NVIDIA adapter driving the monitor processes");
    Expect(P::AdapterRefusal(true, 0x8086) != nullptr, "an Intel-driven monitor is refused");
    Expect(P::AdapterRefusal(true, 0x1002) != nullptr, "an AMD-driven monitor is refused");
    Expect(P::AdapterRefusal(false, P::kNvidia) != nullptr, "an unknown monitor is refused");
}

void Pacing()
{
    // QPC to 100 ns without overflow.
    Expect(Pc::QpcToTicks(10'000'000, 10'000'000) == 10'000'000, "QPC at 10 MHz is already 100 ns");
    Expect(Pc::QpcToTicks(3'000'000, 3'000'000) == 10'000'000, "one second at 3 MHz");
    const int64_t year = int64_t(365) * 24 * 3600;
    Expect(Pc::QpcToTicks(year * 10'000'000 + 5, 10'000'000) == year * 10'000'000 + 5, "a year of uptime, exact");
    Expect(Pc::QpcToTicks(year * 24'000'000, 24'000'000) == year * 10'000'000, "a year at 24 MHz, no overflow");
    Expect(Pc::QpcToTicks(-1, 10) == 0 && Pc::QpcToTicks(5, 0) == 0, "invalid clocks read 0");

    Pc::Pacer pacer;
    Pc::FrameDecision d = pacer.Next(0, false, false);
    Expect(d.verdict == Pc::Verdict::Drop, "an unstamped frame is dropped");
    d = pacer.Next(1'000'000, false, false);
    Expect(d.verdict == Pc::Verdict::TakeNewStream && Same(d.reason, "the first frame"), "the first frame starts");
    d = pacer.Next(1'166'667, false, false);
    Expect(d.verdict == Pc::Verdict::Take, "60 Hz continues the stream");
    Expect(pacer.Next(1'166'667, false, false).verdict == Pc::Verdict::Drop, "the same stamp is dropped");
    Expect(pacer.Next(1'100'000, false, false).verdict == Pc::Verdict::Drop, "an older stamp is dropped");
    Expect(pacer.Last() == 1'166'667, "a dropped frame does not move the clock");
    d = pacer.Next(1'166'667 + Pc::kStreamGap, false, false);
    Expect(d.verdict == Pc::Verdict::Take, "exactly 100 ms still continues");
    d = pacer.Next(1'166'667 + 2 * Pc::kStreamGap + 1, false, false);
    Expect(d.verdict == Pc::Verdict::TakeNewStream && Same(d.reason, "more than 100 ms since the last frame"),
           "a longer gap restarts");
    d = pacer.Next(pacer.Last() + 1, true, false);
    Expect(d.verdict == Pc::Verdict::TakeNewStream && Same(d.reason, "the source's size or crop changed"),
           "geometry restarts");
    d = pacer.Next(pacer.Last() + 1, true, true);
    Expect(d.verdict == Pc::Verdict::TakeNewStream && Same(d.reason, "resumed after a pause"), "resume restarts");

    Pc::CropWait wait;
    Expect(!wait.Waiting(), "not waiting at start");
    Expect(wait.Failed(5000), "the first unconfirmed crop is dropped");
    Expect(wait.Failed(5999), "dropped for up to a second");
    Expect(!wait.Failed(6000), "a second later the session stops");
    wait.Confirmed();
    Expect(!wait.Waiting() && wait.Failed(9000), "a confirmed crop restarts the wait");

    Pc::AgeStats ages;
    Expect(ages.Percentile(50) == 0.0, "no samples, 0");
    for (int i = 1; i <= 100; ++i)
        ages.Add(double(i));
    Expect(ages.Percentile(50) == 50.0 && ages.Percentile(95) == 95.0 && ages.Percentile(100) == 100.0,
           "nearest-rank percentiles");
    Expect(ages.Percentile(0) == 1.0, "p0 is the minimum");
    ages.Add(-1.0);
    ages.Add(std::nan(""));
    ages.Add(120'000.0);
    Expect(ages.Count() == 100, "clock faults are not ages");
    for (size_t i = 0; i < Pc::AgeStats::kWindow; ++i)
        ages.Add(7.0);
    Expect(ages.Count() == Pc::AgeStats::kWindow && ages.Percentile(95) == 7.0, "a bounded window of recent ages");
}

Pr::Parsed Parse(std::vector<std::string> args) { return Pr::ParseArguments(args); }

void Protocol()
{
    Expect(Parse({}).ok && Parse({}).options.mode == Pr::Mode::Help, "no arguments: help");
    Expect(Parse({ "--help" }).options.mode == Pr::Mode::Help, "--help");
    Expect(Parse({ "--catalog" }).options.mode == Pr::Mode::Catalog, "--catalog");
    Pr::Parsed p = Parse({ "--run", "--window", "0x1A2b3C", "--max-frames", "600", "--trace" });
    Expect(p.ok && p.options.mode == Pr::Mode::Run && p.options.window == 0x1A2B3C && p.options.maxFrames == 600 &&
               p.options.trace && !p.options.noNr && p.options.vsync,
           "--run --window with options");
    p = Parse({ "--run", "--countdown", "5", "--no-nr", "--no-vsync" });
    Expect(p.ok && p.options.countdown == 5 && p.options.noNr && !p.options.vsync, "--run --countdown");
    const std::vector<std::vector<std::string>> refused = {
        { "--run" },
        { "--run", "--window", "0x10", "--countdown", "3" },
        { "--run", "--window" },
        { "--run", "--window", "1234" },
        { "--run", "--window", "0x" },
        { "--run", "--window", "0x0" },
        { "--run", "--window", "0xZZ" },
        { "--run", "--window", "0x1FFFFFFFFFFFFFFFF" },
        { "--run", "--countdown", "0" },
        { "--run", "--countdown", "61" },
        { "--run", "--countdown", "-1" },
        { "--run", "--countdown", "5", "--max-frames", "0" },
        { "--run", "--countdown", "5", "--max-frames" },
        { "--catalog", "--run", "--countdown", "5" },
        { "--catalog", "--trace" },
        { "--help", "--catalog" },
        { "--window", "0x10" },
        { "--run", "--countdown", "5", "--fullscreen" },
    };
    for (const auto& args : refused)
    {
        const Pr::Parsed r = Parse(args);
        std::string what = "refused:";
        for (const auto& a : args)
            what += " " + a;
        Expect(!r.ok && !r.error.empty(), what.c_str());
    }

    // Events: one line, escaped, numbers as numbers.
    const std::string line = Pr::Event("started", { { "window", Pr::HandleText(0xABC) },
                                                    { "title", std::string("Say \"hi\"\\\n\t\x01 ação") },
                                                    { "width", uint64_t(1280) },
                                                    { "delta", int64_t(-3) },
                                                    { "age_ms", 12.3456 },
                                                    { "nr", true },
                                                    { "reason", static_cast<const char*>(nullptr) },
                                                    { "bad", std::nan("") } });
    Expect(line == "{\"event\":\"started\",\"window\":\"0xABC\",\"title\":\"Say \\\"hi\\\"\\\\\\n\\t\\u0001 ação\","
                   "\"width\":1280,\"delta\":-3,\"age_ms\":12.346,\"nr\":true,\"reason\":null,\"bad\":null}\n",
           "event encoding");
    Expect(line.find('\n') == line.size() - 1, "one event, one line");

    // Commands.
    Pr::Request r = Pr::ParseCommand("{\"cmd\":\"stop\"}");
    Expect(r.command == Pr::Command::Stop && !r.haveRevision, "stop");
    r = Pr::ParseCommand("  { \"rev\" : 42 , \"cmd\" : \"status\" }\r\n");
    Expect(r.command == Pr::Command::Status && r.haveRevision && r.revision == 42, "status with a revision");
    Expect(Pr::ParseCommand("{\"cmd\":\"quit\",\"force\":true}").command == Pr::Command::Stop, "quit is stop");
    r = Pr::ParseCommand("{\"cmd\":\"set-model\",\"path\":\"C:\\\\m.dll\"}");
    Expect(r.command == Pr::Command::Unknown && r.name == "set-model", "unknown commands are named, not acted on");
    const char* invalid[] = {
        "",
        "stop",
        "{",
        "{\"cmd\":\"stop\"",
        "{\"cmd\":\"stop\"} trailing",
        "{\"cmd\":\"stop\",}",
        "{\"cmd\" \"stop\"}",
        "{\"cmd\":\"stop\",\"cmd\":\"status\"}",
        "{\"rev\":1,\"rev\":2,\"cmd\":\"stop\"}",
        "{\"rev\":-1,\"cmd\":\"stop\"}",
        "{\"rev\":1.5,\"cmd\":\"stop\"}",
        "{\"rev\":99999999999999999999999,\"cmd\":\"stop\"}",
        "{\"cmd\":[\"stop\"]}",
        "{\"cmd\":{\"x\":1}}",
        "{\"cmd\":\"st\\u006fp\"}",
        "{\"cmd\":\"st\nop\"}",
        "{\"other\":\"x\"}",
        "[\"stop\"]",
    };
    for (const char* text : invalid)
    {
        const Pr::Request bad = Pr::ParseCommand(text);
        Expect(bad.command == Pr::Command::Invalid && !bad.error.empty(), text);
    }
    std::string huge = "{\"cmd\":\"stop\",\"pad\":\"" + std::string(Pr::kMaxCommandBytes, 'x') + "\"}";
    Expect(Pr::ParseCommand(huge).command == Pr::Command::Invalid, "an oversized command is refused");
    Expect(std::strcmp(Pr::kSeamEnvironment, "RETOQUE_WINDOW_HOST") == 0 && Pr::kSeamVersion == 1,
           "the seam's variable and version");
    Expect(int(Pr::Exit::RestartRequired) == 7 && int(Pr::Exit::SeamRefused) == 5, "exit codes are the contract");
}
} // namespace

int main()
{
    Geometry();
    Policy();
    Pacing();
    Protocol();
    if (failures != 0)
    {
        std::printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("PASS: window host rules: %d checks (crop, candidates, identity, pause/stop, adapter, pacing, "
                "source age, command line, events, commands)\n",
                checks);
    return 0;
}
