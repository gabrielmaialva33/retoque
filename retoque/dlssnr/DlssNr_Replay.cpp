#include "pch.h"

// The offline replay adapter (docs/dlssnr/design/offline-replay-seam.md): recorded NR-boundary frames enter the same
// EvaluateAfterUpscale a game's DLSS evaluate reaches. Exported unmangled from this fork-owned unit, outside
// Source.def, and inert unless the process asked for replay with RETOQUE_REPLAY=1.

#include "DlssNr_Replay.h"
#include "DlssNr_ReplayClock.h"
#include "DlssNrFeature_Dx12.h"

#include <Logger.h>
#include <State.h>

#include <cstring>

namespace
{
DlssNr::Replay::Outcome g_outcome;
bool g_session = false;

bool ProcessAskedForReplay()
{
    char flag[4] {};
    const DWORD length = GetEnvironmentVariableA("RETOQUE_REPLAY", flag, sizeof(flag));
    return length == 1 && flag[0] == '1';
}
} // namespace

namespace DlssNr::Replay
{
void NoteSkip(const char* reason)
{
    if (!ReplayClock::Active() || reason == nullptr || g_outcome.skip[0] != '\0')
        return;
    strncpy_s(g_outcome.skip, reason, _TRUNCATE);
}

void NoteDispatch(bool recorded, bool carried, uint32_t interval, uint32_t requested, uint32_t completed,
                  int32_t modelResult)
{
    if (!ReplayClock::Active())
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
    if (g_session || !ProcessAskedForReplay())
        return 0;
    g_session = true;
    DlssNr::ReplayClock::Begin();
    LOG_WARN("DLSS-NR replay: offline replay session started (RETOQUE_REPLAY=1); decisions follow the logical clock");
    return 1;
}

extern "C" __declspec(dllexport) int RetoqueReplay_Evaluate(ID3D12GraphicsCommandList* cmdList,
                                                            NVSDK_NGX_Parameter* params, ID3D12CommandQueue* queue,
                                                            const DlssNr::Replay::Frame* frame,
                                                            DlssNr::Replay::Outcome* outcome)
{
    if (!g_session || frame == nullptr || outcome == nullptr || frame->version != 1 || outcome->version != 1)
        return 0;

    State::Instance().frameCount = frame->present;
    DlssNr::ReplayClock::Set(frame->timeMs);
    g_outcome = {};
    DlssNr::EvaluateAfterUpscale(cmdList, params, queue, false, frame->rayReconstruction != 0);
    *outcome = g_outcome;
    return 1;
}

extern "C" __declspec(dllexport) void RetoqueReplay_End()
{
    if (!g_session)
        return;
    DlssNr::ReplayClock::End();
    g_session = false;
    LOG_INFO("DLSS-NR replay: session ended");
}
