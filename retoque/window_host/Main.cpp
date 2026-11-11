#include "pch.h"

// RetoqueWindowHost: NR on any window, by capture, with no hook in the captured program
// (docs/dlssnr/design/nr-anything.md, phase 2: preview output). It captures one top-level window with Windows Graphics
// Capture, carries each frame from D3D11 to D3D12 through shared textures and fences, copies it into a swapchain of its
// own, and hands that swapchain to the production OptiScaler.dll's present pass through the RetoqueWindow_* seam before
// each Present. The command line, exit codes and JSON lines are Protocol.h's.
//
// Lifetime: a GPU object is released only after the fence that proves its last use has completed. If that cannot be
// proved within the timeouts, or the device is lost, the host abandons every object, says "restart-required" and ends
// the process at once (exit 7) rather than release anything the GPU may still use.

#include "Capture.h"
#include "Devices.h"
#include "NrSeam.h"
#include "Output.h"
#include "Pacing.h"
#include "Policy.h"
#include "Protocol.h"
#include "Sources.h"
#include "Transport.h"

#include <winrt/base.h>

#include <deque>
#include <fcntl.h>
#include <io.h>
#include <mutex>
#include <thread>
#include <vector>

namespace RetoqueWindowHost
{
namespace
{
namespace Pr = Protocol;
using Pr::Event;
using Pr::Exit;

constexpr DWORD kDrainMs = 2000;
constexpr DWORD kLatencyWaitMs = 100;
constexpr DWORD kTickMs = 50;

void Emit(const std::string& line)
{
    fwrite(line.data(), 1, line.size(), stdout);
    fflush(stdout);
}

std::wstring Widen(const std::string& text)
{
    if (text.empty())
        return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(chars > 0 ? chars : 0), L'\0');
    if (chars > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), chars);
    return out;
}

// stdin, read on a thread of its own and handed over line by line. The thread may stay blocked in ReadFile until the
// process exits; it owns nothing.
struct Commands
{
    std::mutex mutex;
    std::deque<std::string> lines;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    void Start()
    {
        std::thread(
            [this]
            {
                HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
                if (in == nullptr || in == INVALID_HANDLE_VALUE)
                    return;
                std::string pending;
                char buffer[1024];
                DWORD read = 0;
                while (ReadFile(in, buffer, sizeof(buffer), &read, nullptr) && read > 0)
                {
                    pending.append(buffer, read);
                    size_t newline;
                    while ((newline = pending.find('\n')) != std::string::npos)
                    {
                        Push(pending.substr(0, newline));
                        pending.erase(0, newline + 1);
                    }
                    // A line that never ends is refused as oversized, not buffered without bound.
                    if (pending.size() > Pr::kMaxCommandBytes)
                    {
                        Push(pending);
                        pending.clear();
                    }
                }
            })
            .detach();
    }

    void Push(std::string line)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            lines.push_back(std::move(line));
        }
        SetEvent(event);
    }

    bool Pop(std::string& line)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (lines.empty())
            return false;
        line = std::move(lines.front());
        lines.pop_front();
        return true;
    }
};

int Catalog()
{
    size_t refused = 0;
    const auto entries = Sources::Catalog(refused);
    for (const auto& e : entries)
        Emit(Event("window", { { "hwnd", Pr::HandleText(e.identity.hwnd) },
                               { "pid", uint64_t(e.identity.pid) },
                               { "title", e.title },
                               { "class", e.identity.className },
                               { "executable", e.identity.executable },
                               { "width", int64_t(e.width) },
                               { "height", int64_t(e.height) } }));
    Emit(Event("catalog", { { "windows", uint64_t(entries.size()) }, { "excluded", uint64_t(refused) } }));
    return int(Exit::Ok);
}

int Refuse(Exit code, const char* stage, const std::string& why)
{
    Emit(Event("refused", { { "stage", stage }, { "reason", why }, { "exit", int64_t(code) } }));
    return int(code);
}

struct Counters
{
    uint64_t captured = 0, superseded = 0, dropped = 0, late = 0, presented = 0, nrEntered = 0, nrRecorded = 0;
    uint64_t streams = 0;
    Pacing::AgeStats ages;
};

std::vector<Pr::Field> Summary(const Counters& c)
{
    return { { "captured", c.captured },
             { "superseded", c.superseded },
             { "dropped", c.dropped },
             { "late", c.late },
             { "presented", c.presented },
             { "nr_entered", c.nrEntered },
             { "nr_recorded", c.nrRecorded },
             { "streams", c.streams },
             { "age_p50_ms", c.ages.Percentile(50) },
             { "age_p95_ms", c.ages.Percentile(95) } };
}

// Abandon, never release: the GPU may still use what the host would free.
[[noreturn]] void RestartRequired(const std::string& why, Capture& capture, Transport& transport, Output& output,
                                  const Counters& counters)
{
    auto fields = Summary(counters);
    fields.insert(fields.begin(), Pr::Field { "reason", why });
    Emit(Event("restart-required", fields));
    capture.Abandon();
    transport.Abandon();
    output.Abandon();
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), UINT(Exit::RestartRequired));
    for (;;)
        Sleep(INFINITE);
}
} // namespace

int RunCapture(const Pr::Options& options)
{
    std::string why;
    if (!Capture::Supported(why))
        return Refuse(Exit::Unsupported, "capture", why);

    HWND source = reinterpret_cast<HWND>(options.window);
    if (options.countdown > 0)
    {
        for (int left = options.countdown; left > 0; --left)
        {
            Emit(Event("countdown", { { "seconds", int64_t(left) } }));
            Sleep(1000);
        }
        source = GetForegroundWindow();
    }
    const Policy::WindowFacts facts = Sources::Facts(source);
    if (const char* refusal = Policy::CandidateRefusal(facts))
        return Refuse(Exit::SourceRefused, "source", refusal);
    Policy::Identity identity;
    if (!Sources::ReadIdentity(source, identity))
        return Refuse(Exit::SourceRefused, "source", "the window's process could not be identified");
    const std::string title = Sources::Title(source);

    // Every DXGI and D3D object the host will use exists before OptiScaler.dll is loaded (Phase 1 audit).
    Devices devices;
    if (!devices.CreateFactory(why))
        return Refuse(Exit::Unsupported, "devices", why);
    const Sources::AdapterChoice adapter = Sources::AdapterFor(source, devices.factory.Get());
    if (const char* refusal = Policy::AdapterRefusal(adapter.outputFound, adapter.vendorId);
        refusal != nullptr && !(options.noNr && adapter.outputFound))
        return Refuse(Exit::SourceRefused, "adapter", refusal);
    if (!devices.Create(adapter.adapter.Get(), why))
        return Refuse(Exit::DeviceFailed, "devices", why);

    Geometry::CropInputs crop0;
    uint32_t width = 640, height = 360;
    if (Sources::CropInputs(source, crop0) && !crop0.client.Empty())
    {
        width = uint32_t(crop0.client.Width());
        height = uint32_t(crop0.client.Height());
    }
    Transport transport;
    if (!transport.Create(devices.d3d11.Get(), devices.context.Get(), devices.d3d12.Get(), why) ||
        !transport.EnsureExtent(width, height, why))
        return Refuse(Exit::DeviceFailed, "transport", why);
    Output output;
    if (!output.Create(devices.factory.Get(), devices.d3d12.Get(), devices.queue.Get(), width, height,
                       L"Retoque: NR on any window (preview) - " + Widen(title), why))
        return Refuse(Exit::DeviceFailed, "preview", why);

    NrSeam seam;
    if (!options.noNr && !seam.Load(why))
        return Refuse(Exit::SeamRefused, "nr", why);

    // Never closed: a FrameArrived callback racing the capture's revoke may still signal it.
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Capture capture;
    if (wake == nullptr || !capture.Start(source, devices.d3d11.Get(), true, wake, why))
        return Refuse(Exit::SourceRefused, "capture", why.empty() ? std::string("no wake event") : why);

    Commands commands;
    commands.Start();
    Emit(Event("started", { { "protocol", uint64_t(Pr::kProtocolVersion) },
                            { "window", Pr::HandleText(identity.hwnd) },
                            { "pid", uint64_t(identity.pid) },
                            { "title", title },
                            { "executable", identity.executable },
                            { "adapter", adapter.name },
                            { "width", uint64_t(width) },
                            { "height", uint64_t(height) },
                            { "nr", seam.Loaded() },
                            { "dll", Sources::Narrow(seam.Path()) } }));

    LARGE_INTEGER frequency {};
    QueryPerformanceFrequency(&frequency);
    Counters counters;
    Pacing::Pacer pacer;
    Pacing::CropWait cropWait;
    Geometry::Box lastBox {};
    bool paused = false, resumed = false;
    std::string nrStatus, stopReason;
    int exitCode = int(Exit::Ok);

    auto status = [&](bool haveRevision, uint64_t revision)
    {
        auto fields = Summary(counters);
        fields.insert(fields.begin(), { { "state", paused ? "paused" : "running" }, { "nr_status", nrStatus } });
        if (haveRevision)
            fields.insert(fields.begin(), Pr::Field { "rev", revision });
        Emit(Event("status", fields));
    };

    while (stopReason.empty())
    {
        const HANDLE handles[] = { wake, commands.event };
        MsgWaitForMultipleObjectsEx(2, handles, kTickMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::string line;
        while (stopReason.empty() && commands.Pop(line))
        {
            const Pr::Request request = Pr::ParseCommand(line);
            if (request.command == Pr::Command::Status)
                status(request.haveRevision, request.revision);
            else if (request.command == Pr::Command::Stop)
                stopReason = "stopped by a command";
            else
                Emit(Event("command-refused",
                           { { "cmd", request.name },
                             { "reason", request.command == Pr::Command::Unknown ? std::string("unknown command")
                                                                                 : request.error } }));
        }
        if (!stopReason.empty())
            break;
        if (output.CloseRequested())
        {
            stopReason = "the preview window was closed";
            break;
        }
        if (devices.Lost())
            RestartRequired("the device was lost", capture, transport, output, counters);
        capture.Retire(transport.CapturedCompleted());

        const Policy::Decision decision =
            Policy::DecideSource(Sources::SourceState(source, identity, capture.ItemClosed()));
        if (decision.action == Policy::Action::Stop)
        {
            stopReason = decision.reason;
            break;
        }
        if (decision.action == Policy::Action::Pause)
        {
            if (!paused)
                Emit(Event("paused", { { "reason", decision.reason } }));
            paused = true;
            continue;
        }
        if (paused)
        {
            Emit(Event("resumed"));
            paused = false;
            resumed = true;
        }

        CapturedFrame frame;
        if (!capture.Poll(frame, why))
        {
            if (!why.empty())
            {
                stopReason = why;
                exitCode = int(Exit::SourceRefused);
            }
            continue;
        }
        ++counters.captured;
        counters.superseded += frame.superseded;

        Geometry::CropInputs inputs;
        Geometry::Crop crop;
        if (Sources::CropInputs(source, inputs))
        {
            inputs.content = frame.content;
            crop = Geometry::ClientCrop(inputs);
        }
        else
            crop.reason = "the window's geometry could not be read";
        if (!crop.ok)
        {
            ++counters.dropped;
            if (cropWait.Failed(GetTickCount64()))
                continue;
            stopReason = crop.reason;
            exitCode = int(Exit::SourceRefused);
            break;
        }
        cropWait.Confirmed();

        const Pacing::FrameDecision pace = pacer.Next(frame.time, !(crop.box == lastBox), resumed);
        if (pace.verdict == Pacing::Verdict::Drop)
        {
            ++counters.dropped;
            continue;
        }
        resumed = false;
        lastBox = crop.box;
        if (pace.verdict == Pacing::Verdict::TakeNewStream)
        {
            ++counters.streams;
            Emit(Event("stream", { { "reason", pace.reason },
                                   { "width", uint64_t(crop.box.Width()) },
                                   { "height", uint64_t(crop.box.Height()) } }));
        }

        const uint32_t w = crop.box.Width(), h = crop.box.Height();
        if (w != transport.Width() || h != transport.Height() || w != output.Width() || h != output.Height())
        {
            // Nothing may still read the slots or the backbuffers: proved on the CPU, once per resize.
            if (!output.Drain(kDrainMs) || !transport.WaitIdle(kDrainMs))
                RestartRequired("the GPU did not finish before a resize", capture, transport, output, counters);
            capture.Retire(transport.CapturedCompleted());
            bool rebuilt = transport.EnsureExtent(w, h, why);
            if (rebuilt && (output.Width() != w || output.Height() != h))
                rebuilt = output.Resize(w, h, why);
            if (!rebuilt)
            {
                stopReason = why;
                exitCode = int(Exit::DeviceFailed);
                break;
            }
            Emit(Event("resized", { { "width", uint64_t(w) }, { "height", uint64_t(h) } }));
        }

        const uint64_t serial = transport.Submit(frame.texture, crop.box);
        capture.Hold(frame.token, serial);
        const bool ready = output.FrameReady(kLatencyWaitMs);
        if (!output.Consume(transport.Slot(serial), transport.Captured(), transport.Consumed(), serial, ready, why))
            RestartRequired(why, capture, transport, output, counters);
        if (!ready)
        {
            ++counters.late;
            continue;
        }

        DlssNr::WindowHost::Outcome outcome;
        if (seam.Loaded())
        {
            const bool entered = seam.Present(output.Swapchain(), devices.queue.Get(), outcome);
            counters.nrEntered += entered ? 1 : 0;
            counters.nrRecorded += outcome.recorded;
            if (outcome.status != nrStatus)
            {
                nrStatus = outcome.status;
                Emit(Event("nr", { { "entered", entered },
                                   { "running", outcome.running != 0 },
                                   { "passes", outcome.passesRecorded },
                                   { "status", nrStatus } }));
            }
        }
        const HRESULT presented = output.Present(options.vsync);
        if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET || devices.Lost())
            RestartRequired("the device was lost at Present", capture, transport, output, counters);
        ++counters.presented;

        LARGE_INTEGER now {};
        QueryPerformanceCounter(&now);
        const double ageMs = double(Pacing::QpcToTicks(now.QuadPart, frequency.QuadPart) - frame.time) / 10'000.0;
        counters.ages.Add(ageMs);
        if (options.trace)
            Emit(Event("frame", { { "serial", serial },
                                  { "time", int64_t(frame.time) },
                                  { "age_ms", ageMs },
                                  { "nr_recorded", outcome.recorded != 0 },
                                  { "present_hr", int64_t(presented) } }));
        if (options.maxFrames != 0 && counters.presented >= options.maxFrames)
            stopReason = "--max-frames reached";
    }

    // Teardown: no new frames, then every queue and copy proved complete before anything is released.
    capture.Stop();
    if (!output.Drain(kDrainMs) || !transport.WaitIdle(kDrainMs))
        RestartRequired("the GPU did not finish at exit", capture, transport, output, counters);
    capture.Retire(transport.LastSerial());
    seam.End();
    auto fields = Summary(counters);
    fields.insert(fields.begin(), { { "reason", stopReason }, { "exit", int64_t(exitCode) } });
    Emit(Event("stopped", fields));
    return exitCode;
}
} // namespace RetoqueWindowHost

int wmain(int argc, wchar_t** argv)
{
    using namespace RetoqueWindowHost;
    _setmode(_fileno(stdout), _O_BINARY); // JSON lines end in \n, not \r\n
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(Sources::Narrow(argv[i]));
    const Protocol::Parsed parsed = Protocol::ParseArguments(args);
    if (!parsed.ok)
    {
        fputs(Protocol::kUsage, stderr);
        Emit(Protocol::Event(
            "refused",
            { { "stage", "arguments" }, { "reason", parsed.error }, { "exit", int64_t(Protocol::Exit::Usage) } }));
        return int(Protocol::Exit::Usage);
    }
    switch (parsed.options.mode)
    {
    case Protocol::Mode::Help:
        fputs(Protocol::kUsage, stdout);
        return int(Protocol::Exit::Ok);
    case Protocol::Mode::Catalog:
        return Catalog();
    case Protocol::Mode::Run:
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        }
        catch (const winrt::hresult_error&)
        {
            return Refuse(Protocol::Exit::Unsupported, "winrt", "the Windows Runtime could not be initialised");
        }
        return RunCapture(parsed.options);
    }
    return int(Protocol::Exit::Usage);
}
