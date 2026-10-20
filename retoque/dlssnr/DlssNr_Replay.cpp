#include "pch.h"

// The offline replay adapter (docs/dlssnr/design/offline-replay-seam.md): recorded NR-boundary frames enter the same
// EvaluateAfterUpscale a game's DLSS evaluate reaches. Exported unmangled from this fork-owned unit, outside
// Source.def, and inert unless the process asked for replay with RETOQUE_REPLAY=1.
//
// One session per process, owned by the thread that began it. Evaluations are serial and never nested: the logical
// clock, the present counter and the outcome are process-wide, so a second caller is refused rather than allowed to
// interleave with the first.

#include "DlssNr_Replay.h"
#include "DlssNr_ReplayClock.h"
#include "DlssNrFeature_Dx12.h"

#include <Logger.h>
#include <State.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace
{
// Far beyond any recording, small enough to stay exact as microseconds in the clock's int64.
constexpr double kMaxLogicalMs = 1.0e9;

DlssNr::Replay::Outcome g_outcome;
std::atomic<bool> g_session { false };
std::atomic<bool> g_used { false };       // a session ended stays ended: the pass's history is not reset
std::atomic<bool> g_evaluating { false }; // one evaluation at a time, never nested
std::atomic<DWORD> g_owner { 0 };
uint64_t g_lastPresent = 0;         // owner thread only
double g_lastTimeMs = -1.0;         // owner thread only; the first frame may be at 0
thread_local bool t_noting = false; // notes belong to the evaluation running on this thread

bool ProcessAskedForReplay()
{
    char flag[4] {};
    const DWORD length = GetEnvironmentVariableA("RETOQUE_REPLAY", flag, sizeof(flag));
    return length == 1 && flag[0] == '1';
}

bool IsOwner() { return g_session.load(std::memory_order_acquire) && g_owner.load() == GetCurrentThreadId(); }
} // namespace

namespace DlssNr::Replay
{
void NoteSkip(const char* reason)
{
    if (!t_noting || reason == nullptr || g_outcome.skip[0] != '\0')
        return;
    strncpy_s(g_outcome.skip, reason, _TRUNCATE);
}

void NoteDispatch(bool recorded, bool carried, uint32_t interval, uint32_t requested, uint32_t completed,
                  int32_t modelResult)
{
    if (!t_noting)
        return;
    g_outcome.recorded = recorded ? 1u : 0u;
    g_outcome.carried = carried ? 1u : 0u;
    g_outcome.interval = interval;
    g_outcome.requestedPasses = requested;
    g_outcome.completedPasses = completed;
    g_outcome.modelResult = modelResult;
}
} // namespace DlssNr::Replay

extern "C" __declspec(dllexport) uint32_t RetoqueReplay_Version() { return 1; }

extern "C" __declspec(dllexport) int RetoqueReplay_Begin()
{
    if (!ProcessAskedForReplay() || g_used.exchange(true))
        return 0;
    g_owner.store(GetCurrentThreadId());
    g_lastPresent = 0;
    g_lastTimeMs = -1.0;
    DlssNr::ReplayClock::Begin();
    g_session.store(true, std::memory_order_release);
    LOG_WARN("DLSS-NR replay: offline replay session started (RETOQUE_REPLAY=1); decisions follow the logical clock");
    return 1;
}

extern "C" __declspec(dllexport) int RetoqueReplay_Evaluate(ID3D12GraphicsCommandList* cmdList,
                                                            NVSDK_NGX_Parameter* params, ID3D12CommandQueue* queue,
                                                            const DlssNr::Replay::Frame* frame,
                                                            DlssNr::Replay::Outcome* outcome)
{
    if (!IsOwner() || frame == nullptr || outcome == nullptr || frame->version != 1 || outcome->version != 1)
        return 0;
    // Recorded time and presents only move forward; anything else is refused before it touches any state.
    if (!std::isfinite(frame->timeMs) || frame->timeMs < 0.0 || frame->timeMs > kMaxLogicalMs ||
        frame->timeMs <= g_lastTimeMs || frame->present <= g_lastPresent)
        return 0;
    bool idle = false;
    if (!g_evaluating.compare_exchange_strong(idle, true))
        return 0;

    g_lastPresent = frame->present;
    g_lastTimeMs = frame->timeMs;
    State::Instance().frameCount = frame->present;
    DlssNr::ReplayClock::Set(frame->timeMs);
    g_outcome = {};
    t_noting = true;
    DlssNr::EvaluateAfterUpscale(cmdList, params, queue, false, frame->rayReconstruction != 0);
    t_noting = false;
    *outcome = g_outcome;
    g_evaluating.store(false);
    return 1;
}

extern "C" __declspec(dllexport) void RetoqueReplay_End()
{
    if (!IsOwner() || g_evaluating.load())
        return;
    // The end of this process's replay: logical time may be ahead of the real clock it hands back to, and the pass
    // keeps the history the session built. Nothing starts another session here (g_used).
    DlssNr::ReplayClock::End();
    g_session.store(false, std::memory_order_release);
    LOG_INFO("DLSS-NR replay: session ended");
}
