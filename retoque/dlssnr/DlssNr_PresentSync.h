#pragma once

#include <atomic>
#include <optional>

namespace DlssNr
{
// What [DlssNr] PresentSync=auto means for the D3D12 present pass.
//
// The wait exists for frame generation: an interposer that paces the client queue can flip the buffer before the
// pass's write-back retires, and the next pass then re-reads its own output (scottmudge afad5195, nr-present-hook.md).
// Without one, the write-back and the flip are on the same queue, whose order already puts the finished picture on
// screen, and a CPU wait only holds the present thread until the GPU drains. Measured in PCSX2 (present pass, frame
// generation off, emulation unlimited, 2026-10-13, one run each): the pass ran on 106 frames a second with the wait
// and 127 without. So auto keeps the wait whenever frame generation could be involved, and only then.
//
// With one exception since 2026-11-18: FSR-FG's own swapchain at OptiScaler's frame generation hook. The pass is
// submitted on that swapchain's game queue just before its Present, and inside Present the swapchain signals a fence
// on that queue and makes the queue that reads the frame wait for it on the GPU -- FFX's swapchain
// (FrameInterpolationSwapchainDX12.cpp, presentInterpolated and Present) and the fork's presenter, LSFG included
// (MfgSwapChain_Dx12.cpp, the same two places). The order the CPU wait enforced is the swapchain's already, and the
// wait held the present thread for the pass's whole GPU time on every frame: 10 to 16 ms at one pass, 22 to 36 ms at
// two and three (PCSX2 with FSR-FG and LSFG, the present cpu windows of 2026-11-18). Read from the two sources, not
// measured on a GPU: nr-present-hook.md, 2026-11-18, says what that leaves open.
//
// The wait did a second job there, which the exception keeps: tracked (multi-pass) recordings that are admitted one
// at a time refuse the next frame's while this one is unfinished (chain-in-flight.md), and returning only once it
// had finished is what let every frame in.
struct PresentSyncInputs
{
    bool fgHook = false;      // the pass was called from a frame generation present hook
    bool fgOutput = false;    // OptiScaler's own frame generation output is selected
    bool dlssgSeen = false;   // the game set DLSS-G options or a DLSS-G frame count was detected
    bool dlssgLoaded = false; // Streamline's DLSS-G plugin or nvngx_dlssg.dll is loaded in the process
    bool boundary = false;    // the pass ran at the game's own Present, ahead of its DLSS-G

    // The swapchain behind the frame generation hook, for the one that orders the pass itself.
    bool fsrFgOutput = false;         // that output is FSR-FG: FFX's swapchain or the fork's presenter
    bool fsrFgCustomProvider = false; // ...unless a custom FSR-FG provider is named: the swapchain may be its own
    bool onFgGameQueue = false;       // the pass was submitted on the queue that swapchain was created on
    bool serialRecordings = false;    // tracked recordings admitted one at a time: the next waits for this one
};

// The answer and why, one literal per branch: the log says it when it changes.
struct PresentSyncDecision
{
    bool wait = true;
    const char* why = "";
};

inline PresentSyncDecision DecidePresentSync(std::optional<bool> configured, const PresentSyncInputs& in)
{
    if (configured.has_value())
        return { *configured, *configured ? "PresentSync=true" : "PresentSync=false" };

    // At the game's own Present the write-back is on the game's queue ahead of the fence Streamline signals there for
    // DLSS-G, which waits on it before reading the buffer (nr-before-dlssg.md, step 0b, measured in Crimson Desert):
    // the order the wait would enforce is already the queue's. Waiting would only hold the game's present thread.
    if (in.boundary)
        return { false, "auto: at the game's own Present, ahead of its DLSS-G" };

    // DLSS-G paces the client queue from outside whatever swapchain OptiScaler made, FSR-FG's included.
    if (in.dlssgSeen || in.dlssgLoaded)
        return { true, "auto: DLSS-G was seen or is loaded" };

    // Only at the hook, which runs just before that swapchain's Present, and only on the queue its fence is
    // signalled on. Every other output, a custom provider's swapchain, another queue and the wrapped entry below
    // keep the wait.
    if (in.fgHook && in.fsrFgOutput && !in.fsrFgCustomProvider && in.onFgGameQueue)
    {
        // Not for the flip, then, but for the next frame: its recording would be refused while this one runs.
        if (in.serialRecordings)
            return { true, "auto: FSR-FG, with multi-pass recordings admitted one at a time" };

        return { false, "auto: FSR-FG's swapchain orders the game queue's work ahead of its own on the GPU" };
    }

    if (in.fgHook || in.fgOutput)
        return { true, "auto: frame generation whose swapchain is not known to order the pass" };

    return { false, "auto: no frame generation" };
}

inline bool PresentSyncWanted(std::optional<bool> configured, const PresentSyncInputs& in)
{
    return DecidePresentSync(configured, in).wait;
}

// What the last present pass decided, for the menu to show what auto is doing.
inline std::atomic<bool> g_presentSyncWaits { true };
} // namespace DlssNr
