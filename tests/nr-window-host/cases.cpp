// Behaviour of the window-host seam (retoque/dlssnr/DlssNr_WindowHost.cpp) against a scripted present pass.
//
// Two modes, one process each, because a session is once per process:
//   inert    RETOQUE_WINDOW_HOST is not set: every export, in every order, enters nothing and touches no state.
//   session  the variable's other values are refused, then "1" opens the one session and its rules are held.
#include "DlssNrFeature_Dx12.h"
#include "DlssNr_WindowHost.h"
#include "State.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

struct IDXGISwapChain;

extern "C" uint32_t RetoqueWindow_Version();
extern "C" int RetoqueWindow_Begin();
extern "C" int RetoqueWindow_Present(IDXGISwapChain3*, ID3D12CommandQueue*, DlssNr::WindowHost::Outcome*);
extern "C" void RetoqueWindow_End();

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

// Stand-ins with distinct addresses; the seam never dereferences them.
char swapchainObject, queueObject;
auto* const kSwapchain = reinterpret_cast<IDXGISwapChain3*>(&swapchainObject);
auto* const kQueue = reinterpret_cast<ID3D12CommandQueue*>(&queueObject);

// What the fake pass does on its next call.
struct Script
{
    bool record = false;
    bool running = true;
    const char* failure = "";
    bool reenter = false; // the pass calls RetoqueWindow_Present again from inside
    bool endInside = false;
    int reentered = -1;
    std::string reenteredWhy;
} script;
struct Seen
{
    int calls = 0;
    IDXGISwapChain3* swapchain = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    bool fgHook = true, boundary = true;
} seen;
unsigned long long passes = 0;
} // namespace

namespace DlssNr
{
void RunPresentPass(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, bool fgHook, bool boundary)
{
    ++seen.calls;
    seen.swapchain = swapchain;
    seen.queue = queue;
    seen.fgHook = fgHook;
    seen.boundary = boundary;
    if (script.reenter)
    {
        DlssNr::WindowHost::Outcome inner;
        script.reentered = RetoqueWindow_Present(swapchain, queue, &inner);
        script.reenteredWhy = inner.status;
    }
    if (script.endInside)
        RetoqueWindow_End();
    if (script.record)
        ++passes;
}
unsigned long long PassesRecorded() { return passes; }
bool IsRunning() { return script.running; }
const char* FailureReason() { return script.failure; }
const char* HookStatus() { return "present: active on the swapchain's backbuffer"; }
} // namespace DlssNr

namespace
{
int Present(DlssNr::WindowHost::Outcome& outcome) { return RetoqueWindow_Present(kSwapchain, kQueue, &outcome); }

void Inert()
{
    Expect(std::getenv("RETOQUE_WINDOW_HOST") == nullptr, "inert mode runs without the variable");
    Expect(RetoqueWindow_Version() == 1, "ABI version 1");
    DlssNr::WindowHost::Outcome outcome;
    for (int round = 0; round < 3; ++round)
    {
        RetoqueWindow_End();
        Expect(Present(outcome) == 0, "Present refused without the variable");
        Expect(std::strcmp(outcome.status, "no window-host session on this thread") == 0, "and says why");
        Expect(RetoqueWindow_Begin() == 0, "Begin refused without the variable");
        Expect(Present(outcome) == 0, "Present still refused after a refused Begin");
    }
    Expect(seen.calls == 0, "the pass is never entered without the variable");
    Expect(passes == 0 && State::Instance().currentSwapchain == nullptr, "no state touched");
    if (failures == 0)
        std::printf("PASS: window-host seam inert without RETOQUE_WINDOW_HOST: 4 exports, every order, pass never "
                    "entered\n");
}

void Session()
{
    DlssNr::WindowHost::Outcome outcome;
    for (const char* value : { "", "0", "true", "11", "yes", "1 " })
    {
        setenv("RETOQUE_WINDOW_HOST", value, 1);
        Expect(RetoqueWindow_Begin() == 0, "Begin refused unless the variable is exactly 1");
    }
    Expect(Present(outcome) == 0 && seen.calls == 0, "no session from the refused values");

    setenv("RETOQUE_WINDOW_HOST", "1", 1);
    Expect(RetoqueWindow_Begin() == 1, "Begin with RETOQUE_WINDOW_HOST=1");
    Expect(RetoqueWindow_Begin() == 0, "a second Begin is refused");

    // Argument and ABI refusals, before the pass is entered.
    Expect(RetoqueWindow_Present(kSwapchain, kQueue, nullptr) == 0, "no outcome refused");
    outcome.version = 2;
    Expect(Present(outcome) == 0 && outcome.version == 2, "outcome version mismatch refused and left untouched");
    outcome = {};
    Expect(RetoqueWindow_Present(nullptr, kQueue, &outcome) == 0 &&
               std::strcmp(outcome.status, "a present needs the host's swapchain and its queue") == 0,
           "no swapchain refused");
    Expect(RetoqueWindow_Present(kSwapchain, nullptr, &outcome) == 0, "no queue refused");
    Expect(seen.calls == 0, "refusals never enter the pass");

    // Another thread is not the owner.
    int other = -1;
    std::thread(
        [&]
        {
            DlssNr::WindowHost::Outcome mine;
            other = Present(mine);
        })
        .join();
    Expect(other == 0 && seen.calls == 0, "a present from another thread is refused");

    // A present that records: the pass is entered as the wrapped swapchain enters it.
    script = {};
    script.record = true;
    Expect(Present(outcome) == 1 && seen.calls == 1, "Present enters the pass once");
    Expect(seen.swapchain == kSwapchain && seen.queue == kQueue && !seen.fgHook && !seen.boundary,
           "RunPresentPass(swapchain, queue, false)");
    Expect(outcome.version == 1 && outcome.entered == 1 && outcome.recorded == 1 && outcome.running == 1 &&
               outcome.passesRecorded == 1,
           "recorded outcome");
    Expect(std::strcmp(outcome.status, "present: active on the swapchain's backbuffer") == 0, "status is the pass's");

    // A present the pass skipped: nothing carried over from the last outcome.
    script = {};
    Expect(Present(outcome) == 1 && outcome.entered == 1 && outcome.recorded == 0 && outcome.passesRecorded == 1,
           "skipped present outcome");

    // A failed pass says its reason instead of its status.
    script = {};
    script.running = false;
    script.failure = "the model could not be created";
    Expect(Present(outcome) == 1 && outcome.running == 0 &&
               std::strcmp(outcome.status, "the model could not be created") == 0,
           "failure reason reported");

    // A long reason is truncated, never overrun.
    static char longReason[400];
    std::memset(longReason, 'x', sizeof(longReason) - 1);
    script.failure = longReason;
    Expect(Present(outcome) == 1 && std::strlen(outcome.status) == sizeof(outcome.status) - 1,
           "status truncated to its buffer");

    // A swapchain OptiScaler wrapped in this process already runs the pass: refused, not entered.
    struct Slot
    {
        IDXGISwapChain* State::* member;
        const char* what;
    };
    for (const Slot slot : { Slot { &State::currentSwapchain, "currentSwapchain" },
                             Slot { &State::currentWrappedSwapchain, "currentWrappedSwapchain" },
                             Slot { &State::currentFGSwapchain, "currentFGSwapchain" } })
    {
        script = {};
        const int before = seen.calls;
        State::Instance().*slot.member = reinterpret_cast<IDXGISwapChain*>(&swapchainObject);
        Expect(Present(outcome) == 0 && seen.calls == before, slot.what);
        Expect(std::strstr(outcome.status, "OptiScaler wrapped a swapchain") != nullptr, "and says why");
        State::Instance().*slot.member = nullptr;
    }

    // Nested: the inner present is refused, and End from inside the pass is ignored.
    script = {};
    script.reenter = true;
    script.endInside = true;
    Expect(Present(outcome) == 1, "outer present runs");
    Expect(script.reentered == 0 && script.reenteredWhy == "a present is already in the pass",
           "nested present refused");
    script = {};
    Expect(Present(outcome) == 1, "End inside a present did not end the session");

    // End from another thread is ignored; End from the owner ends it for good.
    std::thread([] { RetoqueWindow_End(); }).join();
    Expect(Present(outcome) == 1, "End from another thread ignored");
    const int calls = seen.calls;
    RetoqueWindow_End();
    Expect(Present(outcome) == 0 && seen.calls == calls, "Present refused after End");
    Expect(RetoqueWindow_Begin() == 0, "no second session in the process");
    if (failures == 0)
        std::printf("PASS: window-host seam session: exact variable, one owner, refusals before the pass, outcome per "
                    "present, wrapped swapchains refused, no nesting, one session per process\n");
}
} // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "inert")
        Inert();
    else if (mode == "session")
        Session();
    else
    {
        std::printf("usage: seam inert|session\n");
        return 2;
    }
    return failures == 0 ? 0 : 1;
}
