#include "pch.h"

// NR on any window (docs/dlssnr/design/nr-anything.md, phase 1): the seam through which RetoqueWindowHost.exe runs the
// production D3D12 present pass on a swapchain of its own. Modelled on the offline replay seam (DlssNr_Replay.cpp):
// exported unmangled from this fork-owned unit, outside Source.def, and inert unless the process asked for it with
// RETOQUE_WINDOW_HOST=1. A game never sets that variable; nothing in the DLL calls these exports.
//
// One session per process, owned by the thread that began it; presents are serial and never nested. The pass is
// entered exactly as the wrapped swapchain's Present enters it (RunPresentPass(swapchain, queue, false)), so every
// present-pass setting and fix applies, and nothing here records GPU work or owns a GPU object. Ending a session
// frees nothing: the pass keeps what it built on the host's device until the process exits, as it does in a game
// (DlssNr::Shutdown is not called; see the design's lifetime section).
//
// A process where OptiScaler wrapped a swapchain already reaches the pass through that swapchain's Present. The
// export then refuses, so one frame is never edited twice and the pass's present counters stay one per flip.

#include "DlssNr_WindowHost.h"
#include "DlssNrFeature_Dx12.h"

#include <Logger.h>
#include <State.h>

#include <atomic>
#include <cstring>

namespace
{
std::atomic<bool> g_session { false };
std::atomic<bool> g_used { false };       // one session per process: the pass keeps the history it built
std::atomic<bool> g_presenting { false }; // one present at a time, never nested
std::atomic<DWORD> g_owner { 0 };

bool ProcessAskedForWindowHost()
{
    char flag[4] {};
    const DWORD length = GetEnvironmentVariableA(DlssNr::WindowHost::kEnvironment, flag, sizeof(flag));
    return length == 1 && flag[0] == '1';
}

bool IsOwner() { return g_session.load(std::memory_order_acquire) && g_owner.load() == GetCurrentThreadId(); }

// Any swapchain OptiScaler wrapped in this process: its Present already runs the pass.
bool OptiScalerWrapsASwapchain()
{
    const auto& state = State::Instance();
    return state.currentSwapchain != nullptr || state.currentWrappedSwapchain != nullptr ||
           state.currentFGSwapchain != nullptr;
}

void Say(DlssNr::WindowHost::Outcome* outcome, const char* text)
{
    strncpy_s(outcome->status, text != nullptr ? text : "", _TRUNCATE);
}
} // namespace

extern "C" __declspec(dllexport) uint32_t RetoqueWindow_Version() { return DlssNr::WindowHost::kVersion; }

extern "C" __declspec(dllexport) int RetoqueWindow_Begin()
{
    if (!ProcessAskedForWindowHost() || g_used.exchange(true))
        return 0;
    g_owner.store(GetCurrentThreadId());
    g_session.store(true, std::memory_order_release);
    LOG_WARN("DLSS-NR window host: session started ({}=1); the present pass runs on the host's own swapchain",
             DlssNr::WindowHost::kEnvironment);
    return 1;
}

extern "C" __declspec(dllexport) int RetoqueWindow_Present(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue,
                                                           DlssNr::WindowHost::Outcome* outcome)
{
    if (outcome == nullptr || outcome->version != 1)
        return 0;
    *outcome = {};
    if (!IsOwner())
    {
        Say(outcome, "no window-host session on this thread");
        return 0;
    }
    if (swapchain == nullptr || queue == nullptr)
    {
        Say(outcome, "a present needs the host's swapchain and its queue");
        return 0;
    }
    if (OptiScalerWrapsASwapchain())
    {
        Say(outcome, "OptiScaler wrapped a swapchain in this process; its Present already runs the pass");
        return 0;
    }
    bool idle = false;
    if (!g_presenting.compare_exchange_strong(idle, true))
    {
        Say(outcome, "a present is already in the pass");
        return 0;
    }

    const unsigned long long before = DlssNr::PassesRecorded();
    DlssNr::RunPresentPass(swapchain, queue, false);
    const unsigned long long after = DlssNr::PassesRecorded();

    outcome->entered = 1;
    outcome->recorded = after > before ? 1u : 0u;
    outcome->running = DlssNr::IsRunning() ? 1u : 0u;
    outcome->passesRecorded = after;
    const char* failure = DlssNr::FailureReason();
    Say(outcome, failure != nullptr && failure[0] != '\0' ? failure : DlssNr::HookStatus());
    g_presenting.store(false);
    return 1;
}

extern "C" __declspec(dllexport) void RetoqueWindow_End()
{
    if (!IsOwner() || g_presenting.load())
        return;
    // Frees nothing (see the top of this file). No second session follows in this process (g_used).
    g_session.store(false, std::memory_order_release);
    LOG_INFO("DLSS-NR window host: session ended");
}
